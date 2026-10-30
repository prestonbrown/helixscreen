// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_toolchanger.h"

#include "ui_toast_manager.h"

#include "ams_device_action_value.h"
#include "ams_device_sections.h"
#include "ams_error.h"
#include "ams_fault_event.h"
#include "ams_state.h"
#include "ams_tool_map_sync.h"
#include "app_constants.h"
#include "color_utils.h"
#include "display_numbering.h"
#include "filament_op_slot_resolver.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "lane_legacy_migration.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "lvgl.h"
#include "printer_state.h"
#include "settings_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

// ============================================================================
// Construction / Destruction
// ============================================================================

namespace helix {

AmsBackendToolChanger::AmsBackendToolChanger(IMoonrakerAPI* api, IMoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Initialize system info with tool changer defaults
    system_info_.type = AmsType::TOOL_CHANGER;
    system_info_.type_name = "Tool Changer";

    // Tool changer capabilities
    system_info_.supports_bypass = false; // No bypass on tool changers
    system_info_.has_hardware_bypass_sensor = false;

    spdlog::debug("[AMS ToolChanger] Backend created");
}

void AmsBackendToolChanger::set_discovery(const helix::PrinterDiscovery& discovery) {
    namespace addon = helix::toolchanger_addon;
    set_discovered_tools(discovery.tool_names());
    set_feeder(addon::resolve_feeder(discovery,
                                     helix::SettingsManager::instance().get_feeder_open_macro(),
                                     helix::SettingsManager::instance().get_feeder_close_macro()));
    set_tool_sensor(addon::resolve_tool_sensor(discovery));
    set_tool_commands(addon::resolve_tool_commands(discovery));
    set_tool_feed(addon::resolve_tool_feed(discovery));
    set_mounts_to_feed(addon::feeds_through_mount(discovery));
    set_resume_rule(addon::resume_rule(discovery));
    set_clean_nozzle_gcode(addon::clean_nozzle_gcode(discovery));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        purge_mm_ = helix::SettingsManager::instance().get_mount_feed_purge_mm();
    }
    set_material_source(addon::resolve_material_source(discovery));
    tinfo_macro_present_ = discovery.has_macro("T_INFO");
}

void AmsBackendToolChanger::set_discovered_tools(std::vector<std::string> tool_names) {
    std::lock_guard<std::mutex> lock(mutex_);

    tool_names_ = std::move(tool_names);

    // Initialize tool structures now that we have tool names
    if (!tool_names_.empty()) {
        initialize_tools();
    }

    spdlog::info("[AMS ToolChanger] Set {} discovered tools", tool_names_.size());
}

// Destructor not needed -- base class handles subscription cleanup

// ============================================================================
// Lifecycle Management
// ============================================================================

AmsError AmsBackendToolChanger::additional_start_checks() {
    if (tool_names_.empty()) {
        spdlog::error("[AMS ToolChanger] Cannot start: No tools discovered. "
                      "Call set_discovered_tools() before start()");
        return AmsErrorHelper::not_connected("No tools discovered");
    }

    return AmsErrorHelper::success();
}

void AmsBackendToolChanger::on_started() {
    // Deliberately NOT additional_start_checks(): start() calls that one while
    // holding mutex_, and load_blocking() both blocks the caller and needs
    // mutex_ to publish its result. Doing it there self-deadlocks on a
    // non-recursive mutex the moment a real API is attached - the app hangs at
    // startup, not just the tests. on_started() runs after start() releases the
    // lock, which is why AmsBackendSnapmaker loads from here too.
    //
    register_mount_feed_listener();

    // Per-slot spool metadata. On this backend the store is the ONLY source of
    // filament identity - klipper-toolchanger reports none - so without it a
    // user's colour and material are lost the moment initialize_tools() runs
    // again on rediscovery. T<n> keys (lane_key_style_for) deliberately share
    // the outer key Mainsail #2510 writes, so records interoperate instead of
    // duplicating. Null api_ in unit tests simply leaves the store absent.
    if (!api_) {
        return;
    }
    auto loaded =
        helix::ams::make_loaded_override_store(api_, "toolchanger", get_type(), backend_log_tag());
    if (loaded.store) {
        helix::ams::ingest_legacy_records(*loaded.store, helix::ams::LegacyLockKeys::LaneData,
                                          backend_index());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        override_store_ = std::move(loaded.store);
        overrides_ = std::move(loaded.overrides);
        seed_lane_records_seen_locked();
        // set_discovered_tools() built the slots before start() ran, so they
        // predate everything just loaded. Layer it on now rather than leaving
        // the panel grey until the first status frame arrives.
        if (!system_info_.units.empty()) {
            auto& slots = system_info_.units[0].slots;
            for (size_t i = 0; i < slots.size(); ++i) {
                apply_resolved_lane(slots[i], static_cast<int>(i));
            }
        }
    }

    register_zmod_marker_listener();
    query_zmod_tool_state();
}

void AmsBackendToolChanger::query_zmod_tool_state() {
    // T_INFO answers state and moves nothing, so the query is safe to fire
    // whenever the changer row is Z-Mod's. Its value is the boot case: on
    // firmware whose zmod_color only recomputes active_tool_id inside its own
    // commands, this is what replaces the stale dock-sensor-error boot state
    // with where the tools actually sit.
    if (!api_ || !zmod_c5_row_active() || !tinfo_macro_present_) {
        return;
    }
    // Captureless callbacks only: the query is fire-and-forget (its answer
    // arrives on the console listener), and a failed probe is log-only —
    // claiming error ownership here would mute Klipper's own `!!` line.
    api_->execute_gcode(
        "T_INFO", nullptr,
        [](const MoonrakerError& err) {
            spdlog::debug("[AMS ToolChanger] T_INFO query refused: {}", err.message);
        },
        0, false, nullptr, /*caller_surfaces_errors=*/false);
}

void AmsBackendToolChanger::on_stopping() {
    unregister_zmod_marker_listener();
    unregister_mount_feed_listener();
}

void AmsBackendToolChanger::on_stopped() {
    // Losing the client under a live run strands a hot nozzle: the cleanup
    // goes out while the connection may still carry it, and the run resets.
    // Here and not in on_stopping(): the unwind takes mutex_ and emits, and
    // stop() holds mutex_ across on_stopping().
    abandon_feed_run_after_disconnect();
}

void AmsBackendToolChanger::register_zmod_marker_listener() {
    if (!client_ || !zmod_c5_row_active()) {
        return;
    }

    static const std::string handler_name = "toolchanger_zmod_marker_watcher";
    auto token = lifetime_.token();

    client_->register_method_callback(
        "notify_gcode_response", handler_name, [this, token](const json& msg) {
            // BG THREAD: extract the line into a local; no `this` access here.
            std::string line;
            if (msg.contains("params") && msg["params"].is_array() && !msg["params"].empty() &&
                msg["params"][0].is_string()) {
                line = msg["params"][0].get<std::string>();
            } else if (msg.is_array() && !msg.empty() && msg[0].is_string()) {
                line = msg[0].get<std::string>();
            } else {
                return;
            }

            // BG-side pre-filter (queue-pressure economy): notify_gcode_response
            // fires for EVERY gcode console line — hundreds per second on a busy
            // print. The parsers only act on the swap narration tokens and
            // T_INFO's rows, so anything without one never hits the UpdateQueue.
            if (line.find("Grab attempt for T") == std::string::npos &&
                line.find("Unload attempt for T") == std::string::npos &&
                line.find("_T_IN") == std::string::npos &&
                line.find("_T_OUT") == std::string::npos &&
                line.find(": home") == std::string::npos &&
                line.find(": HEAD") == std::string::npos &&
                line.find("ERROR (both)") == std::string::npos &&
                line.find("Offset:") == std::string::npos) {
                return;
            }

            // MAIN THREAD: on_zmod_marker_line takes mutex_ and writes backend
            // state.
            token.defer("AmsBackendToolChanger::zmod_marker_apply",
                        [this, line = std::move(line)]() mutable { on_zmod_marker_line(line); });
        });

    // FIRMWARE_RESTART reloads the macros; the tool-state query is what
    // re-seeds the carriage reading after klippy comes back.
    client_->register_method_callback(
        "notify_klippy_ready", handler_name, [this, token](const json& /*msg*/) {
            token.defer("AmsBackendToolChanger::zmod_klippy_ready_apply",
                        [this]() { query_zmod_tool_state(); });
        });
}

void AmsBackendToolChanger::unregister_zmod_marker_listener() {
    if (!client_) {
        return;
    }
    client_->unregister_method_callback("notify_gcode_response", "toolchanger_zmod_marker_watcher");
    client_->unregister_method_callback("notify_klippy_ready", "toolchanger_zmod_marker_watcher");
}

void AmsBackendToolChanger::on_zmod_marker_line(const std::string& line) {
    if (auto reading = helix::toolchanger_addon::parse_zmod_marker(line)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Same path a status frame takes: the marker's phase word drives the
            // action, the direction latch and the step bar's index, while its
            // absent fields leave the tool and dock state the object carries alone.
            apply_tool_sensor_locked(*reading);
        }
        emit_event(EVENT_STATE_CHANGED);
        return;
    }

    // T_INFO's rows: one dock/carriage fact per line, and the block-end line is
    // the only place "nothing on the carriage" can be stated.
    bool block_ended = false;
    auto reading = helix::toolchanger_addon::parse_zmod_tinfo_line(line, block_ended);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reading) {
            if (reading->current_tool.has_value()) {
                tinfo_block_saw_head_ = true;
            }
            apply_tool_sensor_locked(*reading);
            changed = true;
        } else if (block_ended) {
            if (!tinfo_block_saw_head_) {
                // Every tool row said home and none said HEAD: the carriage is
                // empty. Reading it here is the point of the query — firmware
                // that only recomputes active_tool_id inside its own commands
                // clears a stale carriage reading through nothing else.
                helix::toolchanger_addon::ToolReading empty;
                empty.current_tool = -1;
                apply_tool_sensor_locked(empty);
                changed = true;
            }
            tinfo_block_saw_head_ = false;
        }
    }
    if (changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

// stop(), release_subscriptions(), is_running() provided by AmsSubscriptionBackend

// ============================================================================
// Event System
// ============================================================================

// set_event_callback() and emit_event() provided by AmsSubscriptionBackend

// ============================================================================
// State Queries
// ============================================================================

AmsSystemInfo AmsBackendToolChanger::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    AmsSystemInfo info = system_info_;
    // system_info_.action stays the hand-kept swap state apply_tool_sensor_locked()
    // reads back to tell a closing grip from a resting gripper; the step under
    // way is what gets published.
    info.action = published_action_locked();
    return info;
}

AmsType AmsBackendToolChanger::get_type() const {
    return AmsType::TOOL_CHANGER;
}

SlotInfo* AmsBackendToolChanger::cached_slot_locked(int slot_index) {
    return system_info_.get_slot_global(slot_index);
}

// get_current_action(), get_current_tool(), get_current_slot(), is_filament_loaded()
// provided by AmsSubscriptionBackend

bool AmsBackendToolChanger::slot_is_mounted_locked(int slot_index) const {
    // One tool on the carriage at a time, and "unload" here means UNSELECT_TOOL —
    // an unmount, not a filament retraction. Unmounting a docked tool is
    // meaningless, so only the carriage tool qualifies. See the header note for
    // why the inherited is_present() rule cannot express this.
    if (slot_index < 0 || slot_index >= system_info_.total_slots) {
        return false;
    }
    // Sensors that cannot identify the mounted tool answer NO for every slot.
    // current_slot still names the last known tool, so without this the carriage
    // slot reads mounted and the menu unmounts against a carriage state nobody
    // can vouch for - exactly what holding the last known tool was meant to
    // prevent.
    if (sensor_error_) {
        return false;
    }
    // Against current_SLOT, not current_tool. On this backend the two are
    // deliberately not interchangeable: ASSIGN_TOOL moves a G-code tool number
    // onto a different physical tool, and klipper-toolchanger reports the
    // ASSIGNED number in toolchanger.tool_number. Comparing a slot index to it
    // offered Unload on the wrong toolhead whenever a remap was in effect.
    return system_info_.current_slot >= 0 && slot_index == system_info_.current_slot;
}

std::optional<bool> AmsBackendToolChanger::head_has_filament(int head) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (head >= 0 && static_cast<size_t>(head) < fed_heads_.size() &&
        fed_heads_[static_cast<size_t>(head)]) {
        return true;
    }
    return std::nullopt;
}

void AmsBackendToolChanger::record_fed_heads(const helix::mount_feed::Run& run) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& head : run.request().heads) {
        if (head.head < 0) {
            continue;
        }
        if (static_cast<size_t>(head.head) >= fed_heads_.size()) {
            fed_heads_.resize(static_cast<size_t>(head.head) + 1, false);
        }
        fed_heads_[static_cast<size_t>(head.head)] = run.request().load;
    }
}

bool AmsBackendToolChanger::can_unload_from_toolhead(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slot_is_mounted_locked(slot_index);
}

bool AmsBackendToolChanger::slot_is_actively_loaded(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // The same fact, asked by the live half of the affordance gate rather than
    // by the snapshot half. Sharing the rule is what makes the withdrawal hold:
    // the two are combined with OR, so an answer computed independently here
    // would put the Unmount button back the moment it disagreed. See the header.
    return slot_is_mounted_locked(slot_index);
}

bool AmsBackendToolChanger::slot_filament_parked_in_toolhead(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mounts_to_feed_ && !sensor_error_ && slot_index >= 0 &&
           slot_index < system_info_.total_slots;
}

std::string AmsBackendToolChanger::unload_blocked_reason(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= system_info_.total_slots) {
        return {};
    }
    // Keyed on the latch, NOT on current_slot. The sensor-error path holds the
    // last known tool on purpose, so current_slot stays >= 0 through the fault
    // and a current_slot < 0 guard would never fire in the case this exists for:
    // a tool is mounted and THEN the docks lose track.
    if (sensor_error_) {
        // Translated HERE, not at the call site: the extractor reads literals,
        // and lv_tr() on a runtime string would leave this untranslatable.
        return lv_tr("Dock sensors cannot tell which tool is mounted");
    }
    return {};
}

// ============================================================================
// Operation Step Bar
// ============================================================================

namespace {

/// One step in a tool changer's operation bar.
///
/// Which steps a machine gets depends on what it reports, so the MODEL and the
/// phase-to-index mapping are both derived from tc_step_sequence() below - but
/// at DIFFERENT times: the model once, when the sidebar builds the bar, and
/// the index on every frame after. Deriving both from the same function does
/// not by itself keep them in sync - the feeder/direction latches the
/// sequence depends on can flip between those two moments (Moonraker
/// republishes only what CHANGED), so a step index recomputed against the
/// CURRENT latches is not guaranteed to match the sequence the bar was
/// actually built from.
///
/// What actually prevents that: get_operation_step_model() snapshots the
/// latches into step_model_feeder_reported_ / step_model_direction_reported_,
/// and resolve_step_locked() resolves against that SAME snapshot
/// rather than the live latches. Once a model exists, its index is pinned to
/// the exact sequence it was built from until the next model build refreshes
/// the snapshot.
enum class TcStep { Release, Dock, Pick, Change, Grip };

/// Steps this machine can actually drive, in order.
///
/// `has_feeder`      - reports `feeder_open` ([medusahc] does)
/// `names_direction` - a phase word distinguishing picking from dropping has
///                     been seen
///
/// Empty means the machine reports no phases at all - plain klipper-toolchanger,
/// or a MedusaHC running the stock macros with no Python extra - and the caller
/// suppresses the bar rather than inventing steps for it.
std::vector<TcStep> tc_step_sequence(StepOperationType op, bool has_feeder, bool names_direction) {
    std::vector<TcStep> steps;
    if (!has_feeder && !names_direction) {
        return steps;
    }
    // The gripper releases before anything moves and re-grips once the new hot
    // end is seated, so it brackets whatever the middle turns out to be.
    if (has_feeder) {
        steps.push_back(TcStep::Release);
    }
    if (names_direction) {
        // A fresh mount has nothing to dock; an unmount never picks anything up.
        if (op != StepOperationType::LOAD_FRESH) {
            steps.push_back(TcStep::Dock);
        }
        if (op != StepOperationType::UNLOAD) {
            steps.push_back(TcStep::Pick);
        }
    } else {
        steps.push_back(TcStep::Change);
    }
    if (has_feeder) {
        steps.push_back(TcStep::Grip);
    }
    return steps;
}

const char* tc_step_label(TcStep step) {
    switch (step) {
    case TcStep::Release:
        return "Release filament";
    case TcStep::Dock:
        return "Dock tool";
    case TcStep::Pick:
        return "Pick up tool";
    case TcStep::Change:
        return "Change tool";
    case TcStep::Grip:
        return "Grip filament";
    }
    return "";
}

/// The coarse action a step projects to. The swap itself is the operation, so
/// every step reads SELECTING except docking, which puts the old tool away.
AmsAction tc_step_action(TcStep step) {
    return step == TcStep::Dock ? AmsAction::UNLOADING : AmsAction::SELECTING;
}

} // namespace

AmsBackend::OperationStepModel
AmsBackendToolChanger::get_operation_step_model(StepOperationType op) const {
    // A run of several tools (main thread, like every feed_run_ access) gets a
    // bar with one stride per tool, so the highlight walks forward through the
    // whole batch instead of restarting at each tool.
    std::vector<int> run_heads;
    if (feed_run_ && feed_run_->request().heads.size() > 1 &&
        ((op != StepOperationType::UNLOAD) == feed_run_->request().load)) {
        for (const auto& head : feed_run_->request().heads) {
            run_heads.push_back(head.head);
        }
    }
    const size_t run_tools = run_heads.size();

    std::lock_guard<std::mutex> lock(mutex_);

    OperationStepModel model;
    // A load or unload through the carriage has its own stages. Mount and
    // Restore are skipped when the tool is already on the carriage.
    if (mounts_to_feed_ && op != StepOperationType::LOAD_SWAP) {
        const bool load = op != StepOperationType::UNLOAD;
        // Each row names the real tool it works and where it falls in the
        // run: "Tool 3 (2/3): Purge" while the second of heads {0, 2, 4} purges.
        const auto tool_label = [this, &run_heads](int tool) {
            if (run_heads.size() < 2 || tool < 0 || static_cast<size_t>(tool) >= run_heads.size()) {
                return std::string();
            }
            return fmt::format(
                "{} ({}/{}): ",
                helix::ui::lane_label(lane_noun(), run_heads[static_cast<size_t>(tool)]), tool + 1,
                run_heads.size());
        };
        int index = 0;
        for (const auto& row : helix::mount_feed::bar_rows(load, run_tools)) {
            using helix::mount_feed::Step;
            // The first tool may already be mounted, and the carriage may need
            // no putting back.
            const bool optional =
                (row.step == Step::Mount && row.tool == 0) || row.step == Step::Restore;
            model.steps.push_back(
                {tool_label(row.tool) + lv_tr(helix::mount_feed::step_label(row.step)), index++,
                 optional,
                 /*live_temp=*/row.step == Step::Heat, helix::mount_feed::step_action(row.step)});
        }
        return model;
    }
    // Snapshot the latches this model is built from: resolve_step_locked()
    // must resolve every frame's index against this SAME sequence, not
    // whatever the latches read by the time a later frame arrives.
    step_model_feeder_reported_ = feeder_state_reported_;
    step_model_direction_reported_ = direction_reported_;
    step_model_captured_ = true;
    const auto sequence =
        tc_step_sequence(op, step_model_feeder_reported_, step_model_direction_reported_);
    int index = 0;
    for (TcStep step : sequence) {
        model.steps.push_back({lv_tr(tc_step_label(step)), index++, false, /*live_temp=*/false,
                               tc_step_action(step)});
    }
    // No phases to show. NOT the legacy Heat/Feed/Purge bar: nothing heats, no
    // filament feeds and nothing purges on a machine that swaps a whole hot end.
    model.suppressed = model.steps.empty();
    return model;
}

lv_subject_t* AmsBackendToolChanger::get_operation_step_index_subject(StepOperationType /*op*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounts_to_feed_ && !feeder_state_reported_ && !direction_reported_) {
        return nullptr;
    }
    // Same route the U1 uses: apply_tool_sensor_locked() writes the step index
    // into system_info_.operation_phase and AmsState mirrors it onto this
    // subject. Static singleton, so the sidebar's member ObserverGuard is right.
    return AmsState::instance().get_ams_operation_phase_subject();
}

std::optional<StepOperationType> AmsBackendToolChanger::running_step_operation() const {
    // Main thread, like every feed_run_ access.
    if (!feed_run_) {
        return std::nullopt;
    }
    return feed_run_->request().load ? StepOperationType::LOAD_FRESH : StepOperationType::UNLOAD;
}

std::string AmsBackendToolChanger::step_heater(int step_index) const {
    if (!feed_run_ || step_index < 0) {
        return {};
    }
    // The same rows get_operation_step_model() lays out for this run.
    const auto& heads = feed_run_->request().heads;
    const auto rows = helix::mount_feed::bar_rows(feed_run_->request().load, heads.size());
    if (step_index >= static_cast<int>(rows.size())) {
        return {};
    }
    const int tool = rows[static_cast<size_t>(step_index)].tool;
    if (tool < 0 || tool >= static_cast<int>(heads.size())) {
        return {};
    }
    return helix::mount_feed::heater_name(heads[static_cast<size_t>(tool)].head);
}

AmsBackendToolChanger::ResolvedStep
AmsBackendToolChanger::resolve_step_locked(const std::string& operation, bool mid_operation) const {
    // Against the operation the sidebar is CURRENTLY rendering: load, unload and
    // swap have different-length sequences, so an index is only meaningful
    // relative to one of them. And, once a model has actually been built,
    // against the SAME feeder/direction snapshot get_operation_step_model()
    // built that sequence from - not the live latches, which can flip mid-
    // operation without the rendered model being rebuilt (see
    // step_model_feeder_reported_ in the header). Before the first model is
    // ever built, there is nothing to stay pinned to, so fall back to the
    // live latches rather than the snapshot's (false, false) power-on default.
    const bool has_feeder =
        step_model_captured_ ? step_model_feeder_reported_ : feeder_state_reported_;
    const bool names_direction =
        step_model_captured_ ? step_model_direction_reported_ : direction_reported_;
    const auto sequence = tc_step_sequence(AmsState::instance().get_active_step_operation(),
                                           has_feeder, names_direction);
    if (sequence.empty()) {
        return {};
    }

    TcStep wanted;
    if (operation == "dropping") {
        wanted = TcStep::Dock;
    } else if (operation == "picking") {
        wanted = TcStep::Pick;
    } else if (operation == "changing") {
        // A controller that names the direction says `changing` first, before
        // it docks or picks: that is the swap's opening step, not a step of
        // its own.
        wanted = names_direction ? sequence.front() : TcStep::Change;
    } else if (operation == "idle") {
        // Both ends of a swap read idle, and the gripper is the only thing that
        // separates them: open is the release that OPENS an operation, closed is
        // the grip that CLOSES it. A machine that never reports the gripper
        // cannot tell those apart, so it gets no step for an idle frame rather
        // than a guessed one.
        //
        // And an idle frame is only ever a STEP while an operation is running.
        // At rest the machine is idle with the gripper closed, which is the same
        // wire state as the closing grip: reporting the last step there leaves
        // operation_phase pinned at it, and AmsState publishes the action before
        // the phase (ams_state.cpp:1453 then :1460), so the next swap's bar
        // observes the stale end index the instant it is built and paints itself
        // complete before the carriage has moved. Snapmaker and AD5X park at -1
        // when idle for the same reason.
        if (!has_feeder || !mid_operation) {
            return {};
        }
        if (feeder_open_) {
            wanted = TcStep::Release;
        } else if (feeder_opened_this_operation_) {
            wanted = TcStep::Grip;
        } else {
            // Closed, mid-operation, and the gripper has not opened yet: this is
            // the gap between dispatching the swap and the machine moving, not
            // the grip that ends one. Claiming the last step here paints the bar
            // complete for the second before anything happens.
            return {};
        }
    } else {
        return {}; // uninitialized / error / anything this build does not know
    }

    for (size_t i = 0; i < sequence.size(); ++i) {
        if (sequence[i] == wanted) {
            return {static_cast<int>(i), tc_step_action(wanted)};
        }
    }
    return {};
}

// ============================================================================
// Path Visualization
// ============================================================================

PathTopology AmsBackendToolChanger::get_topology() const {
    return PathTopology::PARALLEL; // Each tool has its own independent path
}

helix::printer::EndlessSpoolCapabilities
AmsBackendToolChanger::get_endless_spool_capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    using namespace helix::printer;
    if (!zmod_c5_row_active()) {
        return {};
    }
    // Z-Mod's C5 "endless spool mode" (zmod docs, C5PRO.md): when a head runs
    // out, the firmware switches to the next spool whose colour AND type both
    // match. Always on, no toggle to write — the same answer the AD5X IFS
    // backend gives stock zMod.
    EndlessSpoolCapabilities caps;
    caps.availability = EndlessSpoolAvailability::Available;
    caps.provider = "zmod";
    caps.enabled = EndlessSpoolEnabled::On;
    caps.editability = EndlessSpoolEditability::ReadOnly;
    caps.restriction = EndlessSpoolRestriction::FirmwareManaged;
    return caps;
}

PathSegment AmsBackendToolChanger::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // For tool changers, filament segment depends on whether a tool is mounted
    if (system_info_.current_tool >= 0 && system_info_.filament_loaded) {
        return PathSegment::NOZZLE; // Tool is mounted and active
    }
    return PathSegment::SPOOL; // No tool mounted (all tools in docks)
}

PathSegment AmsBackendToolChanger::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (slot_index < 0 || slot_index >= static_cast<int>(tool_mounted_.size())) {
        return PathSegment::NONE;
    }
    const auto* slot = system_info_.get_slot_global(slot_index);
    if (!slot) {
        return PathSegment::NONE;
    }

    // A tool that is not in the machine has no strand to draw. Its dock reads
    // vacant while it is not on the head, which is what stamps EMPTY.
    if (slot->status == SlotStatus::EMPTY) {
        return PathSegment::NONE;
    }

    // PARALLEL multi-toolhead: each tool carries its own hot end and its own
    // filament, so a tool that is present is loaded to its own nozzle whether it
    // is mounted or docked.
    return PathSegment::NOZZLE;
}

std::optional<helix::ErrorEvent> AmsBackendToolChanger::current_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Only the faults this backend owns. Every other ERROR comes from
    // klipper-toolchanger's own status and reads correctly in the generic
    // dialog, so it stays there.
    if (sensor_error_) {
        // The controller's own words when it reported the failure: they name
        // the operation and the tool. Otherwise the sentence unload_blocked_reason()
        // returns, so the dialog and the greyed-out Unmount agree.
        const std::string detail =
            sensor_fault_reported_ && !controller_failure_.empty()
                ? controller_failure_text_locked()
                : std::string(lv_tr("Dock sensors cannot tell which tool is mounted"));
        return helix::make_ams_fault_event(helix::ErrorSource::TOOLCHANGER,
                                           lv_tr("Dock sensors disagree"), detail,
                                           build_recovery_actions());
    }
    if (!afclite_fault_message_.empty()) {
        // The shim's own fault, with the firmware's own wording: a message the
        // printer wrote is the most accurate text the dialog can show. Title is
        // the shared string the Happy Hare and QIDI backends use for the same
        // class of fault.
        return helix::make_ams_fault_event(helix::ErrorSource::TOOLCHANGER,
                                           lv_tr("Filament System Error"), afclite_fault_message_,
                                           build_recovery_actions());
    }
    return std::nullopt;
}

PathSegment AmsBackendToolChanger::infer_error_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (system_info_.action == AmsAction::ERROR) {
        // For tool changers, errors typically occur at the dock or carriage
        return PathSegment::HUB; // Use HUB to represent the docking area
    }
    return PathSegment::NONE;
}

// ============================================================================
// Moonraker Status Update Handling
// ============================================================================

namespace {

/// Whether @p failure is the one already told as @p noticed. A gcode error
/// reply can carry the controller's message with framing around it, so either
/// containing the other counts.
bool failure_already_noticed(const std::string& failure, const std::string& noticed) {
    if (noticed.empty()) {
        return false;
    }
    return failure.find(noticed) != std::string::npos || noticed.find(failure) != std::string::npos;
}

} // namespace

std::string AmsBackendToolChanger::controller_failure_text_locked() const {
    const int target = controller_target_tool_;
    if (target >= 0 && target < static_cast<int>(tool_names_.size())) {
        const std::string tool = target_tool_label_locked(target);
        std::string text = controller_op_is_park_
                               ? fmt::format(fmt::runtime(lv_tr("Parking {} failed: {}")), tool,
                                             controller_failure_)
                               : fmt::format(fmt::runtime(lv_tr("Tool change to {} failed: {}")),
                                             tool, controller_failure_);
        // The controller paused the print; its RESUME re-picks this tool.
        if (recovery_target_locked() == target && print_holds_printer()) {
            text +=
                ". " + fmt::format(fmt::runtime(lv_tr(
                                       "Seat the hot ends, heat {} above {}°C, then tap Resume.")),
                                   tool, resume_rule_.min_nozzle_c);
        }
        return text;
    }
    return fmt::format(fmt::runtime(lv_tr("Tool change failed: {}")), controller_failure_);
}

int AmsBackendToolChanger::recovery_target_locked() const {
    const int target = controller_target_tool_;
    if (resume_rule_.min_nozzle_c <= 0 || target < 0 ||
        target >= static_cast<int>(tool_names_.size())) {
        return -1;
    }
    // The controller's own test: a failed swap, or another tool mounted.
    const int mounted = sensor_tool_reading_.value_or(system_info_.current_tool);
    if (!controller_failure_latched_ && mounted == target) {
        return -1;
    }
    return helix::toolchanger_addon::resume_rule_in_force(resume_rule_) ? target : -1;
}

std::string AmsBackendToolChanger::target_tool_label_locked(int target) const {
    return helix::ui::tool_label_in_form(lane_noun(), tool_names_[static_cast<size_t>(target)],
                                         target, helix::ui::ToolLabelForm::Full);
}

void AmsBackendToolChanger::prepare_for_resume(int /*slot_index*/, ResumeReadyCallback on_ready) {
    int target = -1;
    int min_c = 0;
    std::string tool;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target = recovery_target_locked();
        min_c = resume_rule_.min_nozzle_c;
        if (target >= 0) {
            tool = target_tool_label_locked(target);
        }
    }
    // The machine's RESUME re-picks the target and refuses below its floor;
    // asking first turns a console error into a sentence the screen can show.
    // A heater this side cannot read is left to that refusal.
    if (target >= 0 && api_) {
        const auto& extruders = api_->printer_state().temperature_state().extruders();
        const auto it = extruders.find(helix::mount_feed::heater_name(target));
        if (it != extruders.end() && it->second.temperature <= static_cast<float>(min_c)) {
            spdlog::warn("{} Resume refused: {} reads {:.0f}C, the controller wants above {}C",
                         backend_log_tag(), it->first, it->second.temperature, min_c);
            if (on_ready) {
                on_ready(
                    AmsError(AmsResult::WRONG_STATE,
                             fmt::format("{} at {:.0f}C, at or below {}C", it->first,
                                         it->second.temperature, min_c),
                             fmt::format(fmt::runtime(lv_tr("Heat {} above {}°C before resuming")),
                                         tool, min_c)));
            }
            return;
        }
    }
    if (on_ready) {
        on_ready(AmsErrorHelper::success());
    }
}

void AmsBackendToolChanger::handle_status(const nlohmann::json& params) {
    bool state_changed = false;
    bool sensor_fault = false;
    std::string failure_toast;
    ToastSeverity failure_severity = ToastSeverity::WARNING;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Check for toolchanger object updates
        if (params.contains("toolchanger")) {
            const auto& tc_data = params["toolchanger"];
            if (tc_data.is_object()) {
                spdlog::trace("[AMS ToolChanger] Received toolchanger status update");
                parse_toolchanger_state(tc_data);
                state_changed = true;
            }
        }

        // Dock sensors, when the machine has them, are the physical truth and
        // override whatever parse_toolchanger_state() just took from
        // toolchanger.tool_number. MedusaHC ships verify_tool_pickup: False, so
        // that field is only ever "what SELECT_TOOL last set" - after a failed
        // pickup it names a tool that is not on the head. Applied AFTER the
        // toolchanger parse above so it wins within the same frame.
        if (tool_sensor_.present) {
            if (auto reading = helix::toolchanger_addon::read_tool(params)) {
                apply_tool_sensor_locked(*reading);
                state_changed = true;
            }
        }

        if (material_source_.present) {
            if (auto reading = helix::toolchanger_addon::read_materials(
                    params, static_cast<int>(tool_names_.size()))) {
                apply_material_reading_locked(*reading);
                state_changed = true;
            }
            // The C5 row also watches each afclite lane's spool. Any other
            // tool changer with AFC lanes owes no lane_data re-read for them.
            if (auto spools = helix::toolchanger_addon::read_afc_lane_spools(params)) {
                apply_afc_lane_spools_locked(*spools);
            }
        }

        // Z-Mod's afclite shim: the firmware's own fault flag (a boolean, not a
        // vocabulary to guess at) and its activity word, which is logged for
        // field visibility and acted on for nothing but the fault.
        if (auto afc = helix::toolchanger_addon::read_afc_state(params)) {
            apply_afc_state_locked(*afc);
            state_changed = true;
        }

        // Check for individual tool updates (e.g., "tool T0", "tool T1")
        for (const auto& tool_name : tool_names_) {
            std::string key = "tool " + tool_name;
            if (params.contains(key)) {
                const auto& tool_data = params[key];
                if (tool_data.is_object()) {
                    spdlog::trace("[AMS ToolChanger] Received {} status update", key);
                    parse_tool_state(tool_name, tool_data);
                    state_changed = true;
                }
            }
        }

        // Re-layer what the lane resolves last, so nothing above can undo it. A
        // lane whose only identity is its Spoolman record has no stored
        // override, so this paints whether or not overrides_ holds one.
        if (state_changed && !system_info_.units.empty()) {
            auto& slots = system_info_.units[0].slots;
            for (size_t i = 0; i < slots.size(); ++i) {
                apply_resolved_lane(slots[i], static_cast<int>(i));
            }
        }

        // A mount-to-feed run owns the published operation: the swap frames its
        // mount and restore stages raise must not end it or move its step.
        if (feed_active_) {
            publish_feed_state_locked();
        }
        sensor_fault = std::exchange(feed_sensor_fault_, false);
        failure_toast = std::exchange(pending_failure_toast_, std::string());
        failure_severity = pending_failure_severity_;
    }

    if (!failure_toast.empty()) {
        show_feed_toast(failure_toast, failure_severity);
    }

    // The run reacts to the frame once it is applied: a fault stops it, and a
    // swap waiting on the sensors may now be confirmed.
    if (sensor_fault) {
        on_feed_sensor_fault();
    } else if (feed_swap_check_) {
        recheck_swap();
    }
    // A status frame comes only from a ready Klippy: what a run stopped by a
    // lost connection owes its heaters can go out, once the frames have said
    // whether a print holds the printer.
    if (auto ps = params.find("print_stats"); ps != params.end() && ps->is_object()) {
        if (auto st = ps->find("state"); st != ps->end() && st->is_string()) {
            frame_print_state_ = st->get<std::string>();
            frame_print_state_current_ = true;
        }
    }
    flush_owed_heaters_off();

    // Emit event OUTSIDE the lock to avoid deadlock if the callback
    // queries backend state (e.g., calls get_system_info() which acquires mutex_)
    if (state_changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

int AmsBackendToolChanger::slot_for_tool_locked(int tool) const {
    // A dock reading counts physical docks, and slot i is physical tool i. The
    // G-code tool map does not apply: ASSIGN_TOOL moves a T<n> number, never a
    // hot end.
    if (tool < 0 || tool >= static_cast<int>(tool_names_.size())) {
        return -1;
    }
    return tool;
}

bool AmsBackendToolChanger::raw_sensor_fault_locked() const {
    return sensor_fault_reported_ || sensor_tool_reading_ == -2;
}

void AmsBackendToolChanger::apply_tool_sensor_locked(
    const helix::toolchanger_addon::ToolReading& raw_reading) {
    // A tool past the discovered set names no slot. It is read as a frame that
    // did not name the carriage, so the last known tool stands.
    helix::toolchanger_addon::ToolReading reading = raw_reading;
    if (reading.current_tool && *reading.current_tool >= 0 && !tool_names_.empty() &&
        *reading.current_tool >= static_cast<int>(tool_names_.size())) {
        spdlog::warn("{} Dock sensors report tool {}, outside the {} discovered; ignored",
                     backend_log_tag(), *reading.current_tool, tool_names_.size());
        reading.current_tool.reset();
    }
    // Captured BEFORE the phase branches below overwrite the action. The closing
    // grip of a swap and the resting closed gripper are the same wire state, and
    // the only thing separating them is whether an operation was running.
    // Reading system_info_.action afterwards would always say IDLE and lose that.
    const bool was_mid_operation = pending_dispatch_action_.has_value() ||
                                   system_info_.action == AmsAction::SELECTING ||
                                   system_info_.action == AmsAction::UNLOADING;
    // A mount-to-feed run publishes its own action (LOADING/UNLOADING) over the
    // swap a stage sends, so a stage that swaps counts as a swap on its own.
    // Only for the fault rule: the run republishes the action, phase and step
    // after every frame, so nothing else here decides what it shows.
    // The prompt stage tolerates the same transient: the operator is pushing a
    // strand into a docked hot end, which can jostle a dock switch. A -2 that
    // outlasts it is caught when the Mount is confirmed.
    const bool feed_swap_in_flight = feed_active_ && (feed_swap_in_flight_ || feed_prompt_up_);

    if (reading.current_tool.has_value()) {
        sensor_tool_reading_ = reading.current_tool;
    }
    // Which readings are faults at all is sensor_error_is_fault()'s rule, not
    // this function's. The stored latch follows the same answer: a flag at rest
    // beside a definite carriage is a past failure, and keeping it would make
    // every later raw_sensor_fault_locked() check disagree with the screen.
    const bool fault = helix::toolchanger_addon::sensor_error_is_fault(
        reading, was_mid_operation || feed_swap_in_flight);
    if (reading.last_error) {
        controller_failure_ = *reading.last_error;
        if (controller_failure_.empty()) {
            controller_failure_noticed_.clear();
        }
    }
    if (reading.target_tool) {
        controller_target_tool_ = *reading.target_tool;
    }
    if (reading.sensor_error_named) {
        controller_failure_latched_ = reading.sensor_error_reported;
        sensor_fault_reported_ = reading.sensor_error_reported && fault;
    } else if (reading.current_tool.has_value() && *reading.current_tool >= -1) {
        // The controller's flag latches the last operation and is republished
        // only when the next one clears it. A definite carriage reading that
        // arrives after it is the sensors agreeing again: someone docked or
        // mounted a tool by hand.
        sensor_fault_reported_ = false;
    }

    // -2 means the sensors cannot tell, which is NOT "no tool". Reporting -1
    // would invite a tool change against an unknown carriage state, so hold the
    // last known tool and let the error surface instead.
    if (fault) {
        if (!sensor_error_) {
            spdlog::warn("{} Dock sensors cannot identify the mounted tool", backend_log_tag());
        }
        sensor_error_ = true;
        system_info_.action = AmsAction::ERROR;
        system_info_.operation_detail = "sensor error";
        // A run can no longer trust the carriage. It stops once this frame is
        // applied, outside mutex_.
        if (feed_active_) {
            feed_sensor_fault_ = true;
        }
        // Returns WITHOUT touching current_tool/current_slot: holding the last
        // known tool is the point. Nothing downstream may infer the fault from
        // those fields, because they still name a perfectly plausible tool.
        return;
    }
    // The fault this function raised is withdrawn once the sensors agree again.
    // A changer that publishes no phase word (Z-Mod) has nothing else to end it.
    // A frame silent about the carriage is not the sensors agreeing: the last
    // reading they gave still stands.
    if (!(sensor_error_ && raw_sensor_fault_locked())) {
        if (sensor_error_ && system_info_.action == AmsAction::ERROR &&
            system_info_.operation_detail == "sensor error") {
            system_info_.action = AmsAction::IDLE;
            system_info_.operation_detail.clear();
        }
        sensor_error_ = false;
    }

    // A failure the controller reported but the screen is not showing as a
    // fault - the docks agreed again, or it predates this connection - would
    // otherwise leave no trace of why the tool change did not happen.
    if (!sensor_error_ && controller_failure_latched_ && !controller_failure_.empty() &&
        !failure_already_noticed(controller_failure_, controller_failure_noticed_)) {
        controller_failure_noticed_ = controller_failure_;
        pending_failure_toast_ = controller_failure_text_locked();
        pending_failure_severity_ = ToastSeverity::WARNING;
    }
    // A message with no failed operation behind it (the controller's own
    // startup failing) is about the controller, not a tool change.
    if (!controller_failure_latched_ && !controller_failure_.empty() &&
        !failure_already_noticed(controller_failure_, controller_failure_noticed_)) {
        controller_failure_noticed_ = controller_failure_;
        pending_failure_toast_ =
            fmt::format(fmt::runtime(lv_tr("Tool changer error: {}")), controller_failure_);
        pending_failure_severity_ = ToastSeverity::ERROR;
    }

    // Only when the frame named the carriage. A delta carrying just the gripper
    // or the phase says nothing about which tool is on the head, and answering
    // -1 there withdraws the mounted tool from the unmount gate, its slot status
    // and the active-slot highlight in one pass.
    if (reading.current_tool.has_value() && *reading.current_tool >= -1) {
        const int tool = *reading.current_tool;
        if (system_info_.current_tool != tool) {
            spdlog::info("{} Dock sensors report tool {} (toolchanger said {})", backend_log_tag(),
                         tool, system_info_.current_tool);
        }
        system_info_.current_tool = tool;
        system_info_.current_slot = slot_for_tool_locked(tool);
        system_info_.filament_loaded = (tool >= 0);
    }

    // Dock occupancy, when the frame carried any. Merged rather than replaced:
    // Moonraker republishes only changed fields, so a frame naming two docks
    // says nothing about the third and must not blank it.
    for (size_t i = 0; i < reading.docks.size(); ++i) {
        if (!reading.docks[i].has_value()) {
            continue;
        }
        if (i >= dock_seated_.size()) {
            dock_seated_.resize(i + 1);
        }
        dock_seated_[i] = reading.docks[i];
    }

    refresh_slot_statuses_locked();

    // Gripper state, and whether this machine mentions it at all. Both latched:
    // Moonraker republishes only what CHANGED, so a later frame omitting these
    // is not the machine withdrawing the capability.
    if (reading.feeder_open.has_value()) {
        feeder_open_ = *reading.feeder_open;
        feeder_state_reported_ = true;
    }
    if (feeder_open_) {
        feeder_opened_this_operation_ = true;
    }
    if (!reading.operation.empty() && reading.phase_names_direction) {
        direction_reported_ = true;
    }

    // Set by the idle branch below, consumed after the phase is computed.
    bool operation_ended = false;

    const std::string& op = reading.operation;
    if (!op.empty()) {
        // An operation's first busy word says what it is: MHC_SET opens on
        // `changing`, MHC_DROP goes straight to `dropping`. A park this backend
        // dispatched already said so.
        const bool opens = operation_word_action(op).has_value() &&
                           !operation_word_action(last_operation_word_).has_value();
        if (opens && !pending_dispatch_action_.has_value()) {
            controller_op_is_park_ = op == "dropping";
        }
        last_operation_word_ = op;
        if (op == "idle") {
            operation_idle_reported_ = true;
        }
    }
    if (const auto busy = operation_word_action(op)) {
        system_info_.action = *busy;
        system_info_.operation_detail = op;
        operation_confirmed_ = true;
    } else if (op == "idle") {
        // A swap RELEASES the filament before it moves, so its first frame is
        // idle-with-the-gripper-open. Reporting IDLE there ends the operation in
        // the UI the instant it starts: the step bar is torn down on step 0 and
        // the action buttons come back mid-swap. Hold until the gripper closes.
        //
        // Tied to an operation actually being in flight, because the user can
        // open the gripper by hand from the feeder panel while genuinely idle,
        // and that must NOT read as busy.
        //
        // Bounded by operation_confirmed_: was_mid_operation is partly derived
        // from the action this hold itself sets, so on its own every later
        // idle-with-open frame would re-satisfy the condition that created it
        // and never let go. Once a real dropping/picking/changing frame has
        // confirmed the swap is actually running, a SUBSEQUENT idle frame means
        // it has settled - closed normally, or open because the carriage ended
        // empty with nothing to re-grip - not its own opening frame recurring,
        // so it must be believed.
        const bool holding_opening_frame =
            feeder_state_reported_ && feeder_open_ && was_mid_operation && !operation_confirmed_;
        if (!holding_opening_frame) {
            system_info_.action = AmsAction::IDLE;
            operation_confirmed_ = false;
            // The gripper latch is NOT cleared here. This same frame is the
            // closing grip, and the phase computation below needs the latch to
            // recognise it as such; clearing first made it report no step at the
            // exact moment the bar should light its last one.
            operation_ended = true;
        }
        system_info_.operation_detail = op;
    }

    // Only when the frame actually carried a phase word. Moonraker republishes
    // just the fields that CHANGED, so a delta naming only current_tool - or a
    // pin_watch-only frame, whose reading has no operation at all - would
    // otherwise stomp the phase to -1 in the middle of a swap.
    if (!op.empty()) {
        const ResolvedStep step = resolve_step_locked(op, was_mid_operation);
        system_info_.operation_phase = step.index;
        step_action_ = step.action;
    }

    // Now that the closing grip has had its chance to be recognised: the next
    // operation starts from a closed gripper and must not inherit this one's.
    if (operation_ended) {
        feeder_opened_this_operation_ = false;
    }
}

void AmsBackendToolChanger::apply_material_reading_locked(
    const helix::toolchanger_addon::MaterialReading& reading) {
    if (reading.valid_types) {
        firmware_valid_types_ = *reading.valid_types;
    }
    if (reading.palette) {
        firmware_palette_ = *reading.palette;
    }
    if (!reading.slots) {
        return;
    }
    firmware_slots_seen_ = true;
    bool filament_changed = false;
    bool restated = false;
    for (size_t i = 0; i < reading.slots->size(); ++i) {
        const auto& slot = (*reading.slots)[i];
        if (!slot) {
            continue;
        }
        // A head whose material or colour the firmware now states differently
        // was changed by something; its lane record may have changed with it.
        // The first statement is the boot state, not a change.
        if (i >= firmware_slot_values_.size()) {
            firmware_slot_values_.resize(i + 1);
        }
        std::pair<std::string, std::uint32_t> value{slot->material, slot->rgb.value_or(0)};
        auto& seen = firmware_slot_values_[i];
        if (seen && *seen != value) {
            restated = true;
        }
        seen = std::move(value);
        // The firmware's statement of what this head holds, replaced whole each
        // time the frame carries it.
        helix::ams::Observation vendor(helix::ams::ObservationSource::VendorCache);
        if (!slot->material.empty()) {
            vendor.material = slot->material;
        }
        if (slot->rgb) {
            vendor.color_rgb = *slot->rgb;
        }
        helix::ams::ingest(lane_id(static_cast<int>(i)), vendor);
        if (slot->filament_present) {
            if (i >= head_filament_.size()) {
                head_filament_.resize(i + 1);
            }
            head_filament_[i] = *slot->filament_present;
            filament_changed = true;
        }
    }
    if (filament_changed) {
        refresh_slot_statuses_locked();
    }
    if (restated) {
        schedule_lane_record_reread();
    }
}

void AmsBackendToolChanger::apply_afc_lane_spools_locked(
    const std::vector<helix::toolchanger_addon::AfcLaneSpool>& spools) {
    bool changed = false;
    for (const auto& spool : spools) {
        auto it = afc_lane_spool_seen_.find(spool.lane);
        if (it == afc_lane_spool_seen_.end()) {
            afc_lane_spool_seen_.emplace(spool.lane, spool.spool_id);
        } else if (it->second != spool.spool_id) {
            it->second = spool.spool_id;
            changed = true;
        }
    }
    if (changed) {
        schedule_lane_record_reread();
    }
}

void AmsBackendToolChanger::apply_afc_state_locked(
    const helix::toolchanger_addon::AfcStateReading& afc) {
    // The fault flag is the one field whose semantics need no vocabulary:
    // true is a fault the firmware itself reports, with its own text.
    if (afc.error_state) {
        const std::string text = afc.message.empty() ? "filament system fault" : afc.message;
        if (afclite_fault_message_ != text) {
            spdlog::warn("{} afclite reports a fault: {}", backend_log_tag(), text);
            afclite_fault_message_ = text;
        }
        system_info_.action = AmsAction::ERROR;
        system_info_.operation_detail = text;
        return;
    }
    if (!afclite_fault_message_.empty()) {
        spdlog::info("{} afclite fault cleared", backend_log_tag());
        afclite_fault_message_.clear();
        if (system_info_.action == AmsAction::ERROR) {
            system_info_.action = AmsAction::IDLE;
            system_info_.operation_detail.clear();
        }
    }
    // The activity word's vocabulary beyond "idle" is unverified, so it is
    // logged rather than acted on: the next field bundle captured during a
    // real load/unload shows the vocabulary here, and the mapping that follows
    // is then written against evidence.
    if (afc.current_state != last_afc_state_logged_) {
        spdlog::info("{} afclite state: {}", backend_log_tag(), afc.current_state);
        last_afc_state_logged_ = afc.current_state;
    }
    // The lane map is the firmware's own tool routing (SET_MAP can change it).
    // Logged when it is not identity so a remap is visible in every bundle;
    // nothing acts on it until a capture says what it means for head indices.
    if (!afc.lane_maps.empty()) {
        std::string joined;
        for (size_t i = 0; i < afc.lane_maps.size(); ++i) {
            if (!afc.lane_maps[i].empty() && afc.lane_maps[i] != "T" + std::to_string(i)) {
                joined += (joined.empty() ? "" : " ") + afc.lane_maps[i];
            }
        }
        if (!joined.empty() && joined != last_lane_map_logged_) {
            spdlog::info("{} afclite lane map is not identity: {}", backend_log_tag(), joined);
            last_lane_map_logged_ = joined;
        }
    }
}

std::optional<std::vector<std::string>> AmsBackendToolChanger::get_supported_materials() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!material_source_.present || !firmware_valid_types_ || firmware_valid_types_->empty()) {
        return std::nullopt;
    }
    return firmware_valid_types_;
}

bool AmsBackendToolChanger::firmware_stores_color_and_material(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return material_source_.present && material_source_.write_gcode && firmware_slots_seen_ &&
           firmware_palette_ && !firmware_palette_->empty() && slot_index >= 0 &&
           slot_index < static_cast<int>(tool_names_.size());
}

void AmsBackendToolChanger::parse_toolchanger_state(const nlohmann::json& tc_data) {
    // Parse status: toolchanger.status
    // Values: "ready", "changing", "error", "uninitialized"
    if (tc_data.contains("status") && tc_data["status"].is_string()) {
        std::string status_str = tc_data["status"].get<std::string>();
        const AmsAction status_action = status_to_action(status_str);
        // klipper-toolchanger knows nothing of the docks, so its "ready" says
        // nothing about a dock-sensor fault: only the sensors agreeing ends it.
        if (!(sensor_error_ && status_action == AmsAction::IDLE)) {
            system_info_.action = status_action;
            system_info_.operation_detail = status_str;
        }
        spdlog::trace("[AMS ToolChanger] Status: {} -> {}", status_str,
                      ams_action_to_string(system_info_.action));

        // The toolchanger reporting anything but "ready" means it has taken the
        // dispatched operation over, so its own state machine owns completion
        // from here and the pending macro ack must keep its hands off — a real
        // SELECT_TOOL acks when the macro returns, which is well before the
        // carriage finishes, and forcing IDLE there would report done mid-swap.
        // A re-echoed "ready" is deliberately NOT taking over: that is the
        // no-op case this mechanism exists to catch (#1183). It is harmless
        // anyway — the frame already moved the action to IDLE, so the ack's own
        // value guard drops it.
        if (pending_dispatch_action_.has_value() && system_info_.action != AmsAction::IDLE) {
            spdlog::debug("[AMS ToolChanger] Toolchanger took over the dispatched operation "
                          "({} from status '{}')",
                          ams_action_to_string(system_info_.action), status_str);
            pending_dispatch_action_.reset();
        }
    }

    // Parse current tool: toolchanger.tool_number
    // -1 = no tool selected
    //
    // With dock sensors this is only what SELECT_TOOL last set, and Moonraker
    // sends it in deltas of its own, so it never names the carriage: the
    // sensors do (apply_tool_sensor_locked()).
    if (!tool_sensor_.present && tc_data.contains("tool_number") &&
        tc_data["tool_number"].is_number_integer()) {
        int tool_num = tc_data["tool_number"].get<int>();
        system_info_.current_tool = tool_num;
        // toolchanger.tool_number is the ASSIGNED G-code number of the carriage
        // tool, which ASSIGN_TOOL can move onto any physical tool — it is only
        // the slot index while the mapping is identity. Resolve it through the
        // forward map so the carriage lane is right under a remap too; an
        // unmapped number falls back to itself, the pre-remap answer.
        int seated_slot = -1;
        if (tool_num >= 0) {
            seated_slot = tool_num < static_cast<int>(system_info_.tool_to_slot_map.size())
                              ? system_info_.tool_to_slot_map[static_cast<size_t>(tool_num)]
                              : -1;
            if (seated_slot < 0) {
                seated_slot = tool_num;
            }
        }
        system_info_.current_slot = seated_slot;
        system_info_.filament_loaded = (tool_num >= 0);
        refresh_slot_statuses_locked();
        spdlog::trace("[AMS ToolChanger] Current tool: T{} (physical slot {})", tool_num,
                      seated_slot);
    }

    // Parse tool list: toolchanger.tool_numbers and toolchanger.tool_names
    // This can be used to dynamically update the tool list
    if (tc_data.contains("tool_numbers") && tc_data["tool_numbers"].is_array()) {
        spdlog::trace("[AMS ToolChanger] Tool numbers: {}",
                      helix::json_util::safe_dump(tc_data["tool_numbers"]));
    }
}

void AmsBackendToolChanger::parse_tool_state(const std::string& tool_name,
                                             const nlohmann::json& tool_data) {
    int slot_idx = find_slot_for_tool(tool_name);
    if (slot_idx < 0) {
        spdlog::warn("[AMS ToolChanger] Unknown tool: {}", tool_name);
        return;
    }

    // Parse mounted state: tool.mounted
    //
    // Recorded, but deliberately NOT the source of the slot's LOADED stamp. This
    // is a different Moonraker object from the toolchanger one that assigns the
    // aggregate pair, and an all-tools-mounted payload is a shape we emit
    // ourselves in mock mode — writing `mounted ? LOADED : AVAILABLE` straight
    // into slot.status marked every tool loaded at once (#1199). The stamp is
    // re-derived from the carriage tool instead, so the two writers agree.
    if (tool_data.contains("mounted") && tool_data["mounted"].is_boolean()) {
        bool mounted = tool_data["mounted"].get<bool>();
        if (slot_idx < static_cast<int>(tool_mounted_.size())) {
            tool_mounted_[slot_idx] = mounted;
        }
        refresh_slot_statuses_locked();
        spdlog::trace("[AMS ToolChanger] Tool {} mounted: {}", tool_name, mounted);
    }

    // Parse active state: tool.active
    if (tool_data.contains("active") && tool_data["active"].is_boolean()) {
        bool active = tool_data["active"].get<bool>();
        spdlog::trace("[AMS ToolChanger] Tool {} active: {}", tool_name, active);
    }
}

void AmsBackendToolChanger::refresh_slot_statuses_locked() {
    if (system_info_.units.empty()) {
        return;
    }

    // Without dock sensors a toolhead is always assumed physically there, so the
    // stamp is a straight two-way split on the carriage tool. With them, EMPTY
    // becomes a real answer: a dock reporting vacant for a tool that is not on
    // the head means that hot end has been taken out of the machine.
    auto& slots = system_info_.units[0].slots;
    for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
        // current_slot, not current_tool — `i` indexes physical toolheads, and
        // under an ASSIGN_TOOL remap the carriage tool's G-code number is not
        // its slot index (see the tool_number parse). Comparing against the
        // number stamps LOADED on the lane that merely shares an index with it.
        SlotStatus stamped;
        if (system_info_.current_slot >= 0 && i == system_info_.current_slot) {
            // The mounted tool's own dock always reads vacant — that is where it
            // came from — so the carriage wins over the dock reading.
            stamped = SlotStatus::LOADED;
        } else {
            const bool dock_vacant = i < static_cast<int>(dock_seated_.size()) &&
                                     dock_seated_[static_cast<size_t>(i)].has_value() &&
                                     !*dock_seated_[static_cast<size_t>(i)];
            stamped = dock_vacant ? SlotStatus::EMPTY : SlotStatus::AVAILABLE;
            // A firmware that reports per-head filament (Z-Mod 1.7.4+) upgrades
            // the AVAILABLE branch: a docked head whose slot says it holds no
            // filament reads Empty. It cannot outrank the carriage arm — a
            // mounted head that ran dry is the runout flow's question, not this
            // stamp's — nor a dock-reported vacancy, which says the head itself
            // is out of the machine.
            const bool no_filament = !dock_vacant && i < static_cast<int>(head_filament_.size()) &&
                                     head_filament_[static_cast<size_t>(i)].has_value() &&
                                     !*head_filament_[static_cast<size_t>(i)];
            if (no_filament) {
                stamped = SlotStatus::EMPTY;
            }
        }
        slots[i].status = stamped;

        // Docking is the whole of what this backend senses, and it files no
        // identity record at all: klipper-toolchanger reports none, so the only
        // filament identity a slot ever carries is the override store's, which
        // is a person's statement and not a firmware reading.
        //
        // The dock reading files under tool_docked and leaves present unset:
        // a toolhead in a dock says nothing about the filament in it, so
        // filing presence would make every docked tool read as a loaded bay.
        //
        // The reading filed is the local this pass computed, never
        // slots[i].status. That struct persists across frames and
        // apply_resolved_lane() rewrites it, so anything reading it back is one
        // inserted line away from filing a user's value as a firmware one.
        helix::ams::Observation sensed(helix::ams::ObservationSource::Sensed);
        sensed.tool_docked = stamped != SlotStatus::EMPTY;
        helix::ams::ingest(lane_id(i), sensed);
    }
}

std::optional<AmsAction>
AmsBackendToolChanger::operation_word_action(const std::string& operation) {
    if (operation == "picking") {
        return AmsAction::SELECTING;
    }
    if (operation == "dropping") {
        return AmsAction::UNLOADING;
    }
    // The swap's opening phase (MHC_SET homes before it reads the docks),
    // before the direction is named. SELECTING is the honest generic "a swap
    // is running": every caller treats it as busy, and the only thing that
    // wants the direction is the step bar, which asks separately.
    if (operation == "changing") {
        return AmsAction::SELECTING;
    }
    return std::nullopt;
}

AmsAction AmsBackendToolChanger::status_to_action(const std::string& status) {
    if (status == "ready") {
        return AmsAction::IDLE;
    }
    if (status == "changing") {
        return AmsAction::SELECTING;
    }
    if (status == "error") {
        return AmsAction::ERROR;
    }
    // klipper-toolchanger's own initialize sequence: it homes and moves the
    // carriage, so a tap must not land mid-initialization.
    if (status == "initializing") {
        return AmsAction::RESETTING;
    }
    // 'uninitialized' reads busy, which refuses the tap at the gate. On the
    // default initialize_on: first-use select_tool() would have auto-
    // initialized, so the tap could have cleared the state itself; on
    // initialize_on: manual Klipper rejects it ("Cannot select tool,
    // toolchanger status is uninitialized"), and dispatch_operation()'s error
    // callback unwinds the optimistic SELECTING (#1183). The Reset button,
    // which sends INITIALIZE_TOOLCHANGER, is the way out either way.
    if (status == "uninitialized") {
        return AmsAction::RESETTING;
    }
    return AmsAction::IDLE;
}

void AmsBackendToolChanger::initialize_tools() {
    int tool_count = static_cast<int>(tool_names_.size());

    AmsUnit unit;
    unit.unit_index = 0;
    unit.name = "Tool Changer";
    unit.slot_count = tool_count;
    unit.first_slot_global_index = 0;
    unit.connected = true;
    unit.has_encoder = false;
    unit.has_toolhead_sensor = false;
    unit.has_slot_sensors = false;

    // Initialize slots for each tool
    tool_mounted_.clear();
    tool_mounted_.resize(tool_count, false);

    for (int i = 0; i < tool_count; ++i) {
        SlotInfo slot;
        slot.slot_index = i;
        slot.global_index = i;
        slot.status = SlotStatus::AVAILABLE; // Tools start as available (docked)
        slot.color_rgb = kAmsDefaultSlotColor;
        slot.spool_name = tool_names_[i]; // Use tool name as placeholder

        unit.slots.push_back(slot);
    }

    system_info_.units.clear();
    system_info_.units.push_back(unit);
    system_info_.total_slots = tool_count;

    // Initialize tool-to-slot mapping (1:1 for tool changers) in BOTH
    // directions from one pass: the slot loop above deliberately leaves
    // mapped_tool alone so this is the only writer. ASSIGN_TOOL can move a
    // G-code tool number onto a different physical tool later; when it does,
    // the badge (mapped_tool) and the op-button lane (tool_to_slot_map) have to
    // move together or they name different toolheads.
    system_info_.tool_to_slot_map.clear();
    helix::printer::sync_tool_map_from_forward(system_info_, /*identity_fallback=*/true);

    // A toolchanger frame may already have named the carriage tool before the
    // tool list arrived; re-derive so the fresh slots match it.
    refresh_slot_statuses_locked();

    // initialize_tools() has just reset every slot to default grey with the tool
    // name as a placeholder. That reset IS the wipe: klipper-toolchanger reports
    // no filament identity, so everything a slot shows comes from its lane, and
    // a rediscovery would otherwise throw it away. Re-layer here rather than
    // waiting for the next status frame, so get_slot_info() is never briefly
    // wrong.
    if (!system_info_.units.empty()) {
        auto& slots = system_info_.units[0].slots;
        for (size_t i = 0; i < slots.size(); ++i) {
            apply_resolved_lane(slots[i], static_cast<int>(i));
        }
    }

    tools_initialized_ = true;
    spdlog::info("[AMS ToolChanger] Initialized {} tools", tool_count);
}

int AmsBackendToolChanger::find_slot_for_tool(const std::string& tool_name) const {
    auto it = std::find(tool_names_.begin(), tool_names_.end(), tool_name);
    if (it != tool_names_.end()) {
        return static_cast<int>(std::distance(tool_names_.begin(), it));
    }
    return -1;
}

// ============================================================================
// Operations
// ============================================================================

// NOTE: Must be called while holding mutex_ (accesses system_info_ without lock)
// check_preconditions() provided by AmsSubscriptionBackend

// execute_gcode() provided by AmsSubscriptionBackend

// ============================================================================
// Optimistic dispatch + macro-ack resolution (#1183)
// ============================================================================

uint64_t AmsBackendToolChanger::begin_dispatch_locked(AmsAction action) {
    // A newer dispatch supersedes any older one whose ack is still in flight:
    // that ack presents a stale generation and finalize_dispatch_after_macro()
    // drops it, so it can never resolve the operation now running.
    const uint64_t generation = ++dispatch_generation_;
    pending_dispatch_action_ = action;
    controller_op_is_park_ = action == AmsAction::UNLOADING;

    // A prior swap can reach IDLE without ever passing through the idle/ready
    // branch of apply_tool_sensor_locked() - e.g. a frame carrying only
    // toolchanger.status == "ready" with no addon data in it, parsed by
    // parse_toolchanger_state() alone - which would leave operation_confirmed_
    // stale at true. Left alone, THIS dispatch's own opening idle-with-open
    // frame would fail to hold: a stale "already confirmed" skips the hold
    // just as surely as a genuinely settled swap does.
    operation_confirmed_ = false;

    // Same escape path, and the gripper latch is the more damaging half of it.
    // resolve_step_locked() reads it to tell the grip that ENDS a swap
    // from the closed gripper that precedes one, so a value carried over from
    // the previous operation resolves this dispatch's first closed-gripper
    // frame to the final step - the bar paints itself complete before the
    // carriage has moved, which is the exact failure the latch was added to
    // prevent. apply_tool_sensor_locked() clears it when an operation ends
    // through the idle branch; this covers the ends that never reach it.
    feeder_opened_this_operation_ = false;

    system_info_.action = action;
    // Otherwise the sidebar keeps showing the previous operation's detail (the
    // raw toolchanger status string) until the next frame, which for a no-op
    // never comes.
    system_info_.operation_detail =
        (action == AmsAction::UNLOADING) ? lv_tr("Unloading") : lv_tr("Tool swap");

    spdlog::debug("[AMS ToolChanger] Dispatch #{}: action set optimistically to {}", generation,
                  ams_action_to_string(action));
    return generation;
}

void AmsBackendToolChanger::abandon_dispatch(uint64_t generation) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != dispatch_generation_ || !pending_dispatch_action_.has_value()) {
            return;
        }
        pending_dispatch_action_.reset();
        // A fault a frame raised meanwhile is the machine's answer and stays.
        if (ams_action_holds_system(system_info_.action)) {
            // A swap the controller reports running is someone else's (the
            // rejection is often its "MedusaHC is busy"), and its own idle
            // frame ends it. A source that never says idle cannot end one, so
            // its last word is no evidence of anything still running.
            const auto running = operation_idle_reported_
                                     ? operation_word_action(last_operation_word_)
                                     : std::nullopt;
            if (running) {
                system_info_.action = *running;
                system_info_.operation_detail = last_operation_word_;
            } else {
                system_info_.action = AmsAction::IDLE;
                system_info_.operation_detail.clear();
            }
        }
    }
    emit_event(EVENT_STATE_CHANGED);
}

void AmsBackendToolChanger::finalize_dispatch_after_macro(uint64_t generation) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // "Is the action still mine?" — three ways it can be someone else's:
        //   * a newer dispatch bumped the generation;
        //   * the toolchanger reported a busy status, so parse_toolchanger_state()
        //     handed the operation to klipper-toolchanger's own state machine and
        //     cleared the pending mark;
        //   * something moved the action off the value this dispatch set — a
        //     "ready" frame (which already produced the transition the UI needed)
        //     or an "error" one.
        // In all three the operation is already resolved or is still legitimately
        // running, and forcing IDLE would either lie or truncate it.
        if (generation != dispatch_generation_ || !pending_dispatch_action_.has_value() ||
            system_info_.action != *pending_dispatch_action_) {
            spdlog::debug("[AMS ToolChanger] Macro ack for dispatch #{} resolves nothing (current "
                          "action {}, generation {})",
                          generation, ams_action_to_string(system_info_.action),
                          dispatch_generation_);
            return;
        }

        // The macro ran to completion and the toolchanger never reported doing
        // anything — the "tool T4 already selected" no-op. The gcode ack is the
        // only completion signal that exists for it (#1183).
        spdlog::info("[AMS ToolChanger] Macro complete (gcode ack) with no toolchanger status "
                     "change -> IDLE");
        pending_dispatch_action_.reset();
        system_info_.action = AmsAction::IDLE;
        system_info_.operation_detail.clear();
        changed = true;
    }
    if (changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

AmsError AmsBackendToolChanger::dispatch_operation(std::string gcode, AmsAction action,
                                                   uint32_t timeout_ms) {
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation = begin_dispatch_locked(action);
    }
    // Publish the optimistic action immediately, and OUTSIDE mutex_: the
    // filament panel's completion observer has to see the operation start
    // before it can ever see one end, and the callback may call back in for
    // state (get_current_action() takes mutex_).
    emit_event(EVENT_STATE_CHANGED);

    // A rejected command (the controller's "MedusaHC is busy", a failed swap
    // raised as a gcode error) ends the operation it was meant to start. A reply
    // timeout does not: the macro may still be running, and its frames end it.
    // The `!!` line Klipper broadcasts is what the user sees, so this callback
    // only unwinds.
    AmsError result = ensure_homed_then(
        std::move(gcode), [this, generation]() { finalize_dispatch_after_macro(generation); },
        [this, generation](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                return;
            }
            spdlog::warn("[AMS ToolChanger] Dispatch #{} rejected: {}", generation, err.message);
            abandon_dispatch(generation);
        },
        timeout_ms, /*skip_homing=*/false, /*silent=*/true, /*caller_surfaces_errors=*/false);

    if (!result) {
        // The gcode never left: no IMoonrakerAPI, or the send was refused. No ack
        // will ever arrive, so undo the optimistic action instead of leaving the
        // UI busy and every later operation locked out by is_busy().
        spdlog::warn("[AMS ToolChanger] Dispatch #{} failed to send ({}), reverting optimistic "
                     "action",
                     generation, result.technical_msg);
        abandon_dispatch(generation);
    }
    return result;
}

// ============================================================================

// ============================================================================
// Mount-to-feed
// ============================================================================

bool AmsBackendToolChanger::needs_mount_to_feed(int /*head*/) const {
    return mounts_to_feed_;
}

helix::mount_feed::Request AmsBackendToolChanger::build_feed_request(const std::vector<int>& heads,
                                                                     bool load) {
    namespace mf = helix::mount_feed;
    mf::Request req;
    req.load = load;
    req.heads.reserve(heads.size());
    const int first = heads.front();
    SlotInfo slot;
    int mounted = -1;
    // The next-tool preheat must never lower a heater's target, so each
    // head's current target rides the request. Null API (unit tests) reads 0.
    std::unordered_map<std::string, float> targets;
    if (api_) {
        for (const auto& [name, info] : api_->printer_state().temperature_state().extruders()) {
            targets[name] = info.target;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mounted = system_info_.current_slot;
        for (int head : heads) {
            mf::HeadRequest hr;
            hr.head = head;
            // Only the first tool can already be on the carriage: the run
            // mounts the previous tool before it reaches any later one.
            if (head != mounted || head != first) {
                hr.mount_gcode = mount_gcode(head);
            }
            if (const SlotInfo* lane = system_info_.get_slot_global(head)) {
                slot = *lane;
            } else {
                slot = SlotInfo{};
            }
            hr.temp_c = helix::ui::resolve_load_preheat_material(head, &slot, nullptr)
                            .value_or(helix::ui::PreheatTarget{
                                app_constants::ams::kDefaultLoadPreheatTemp, ""})
                            .temp_c;
            const auto target = targets.find(mf::heater_name(head));
            hr.heater_target_c = target != targets.end() ? static_cast<int>(target->second) : 0;
            req.heads.push_back(std::move(hr));
        }
        // The carriage goes back to whatever the run found on it. A run of one
        // tool that starts mounted leaves it there; several tools always
        // displace the first one, so they always restore.
        if (mounted < 0) {
            req.restore_gcode = park_gcode();
        } else if (mounted != first || heads.size() > 1) {
            req.restore_gcode = mount_gcode(mounted);
        }
        req.restore_head = mounted;
        req.purge_mm = purge_mm_;
    }
    // The prompt is the one part of the run every client of the printer sees,
    // so it is English whatever this screen's language is.
    // The prompt comes before any mount, so a tool it names is still in its
    // dock unless it started on the carriage. A docked strand goes into the
    // hot end; a mounted one in through the extruder.
    req.prompt_title = load ? "Load filament" : "Unload filament";
    std::vector<int> docked;
    std::vector<int> on_carriage;
    for (const mf::HeadRequest& hr : req.heads) {
        (hr.mount_gcode.empty() ? on_carriage : docked).push_back(hr.head + 1);
    }
    const auto numbers = [](const std::vector<int>& tools) {
        std::string list;
        for (int tool : tools) {
            if (!list.empty()) {
                list += ", ";
            }
            list += std::to_string(tool);
        }
        return list;
    };
    const auto docked_phrase = [&]() {
        return docked.size() == 1
                   ? fmt::format("the hot end of Tool {} while it is docked", docked.front())
                   : fmt::format("the hot ends of Tools {} while they are docked", numbers(docked));
    };
    if (on_carriage.empty()) {
        req.prompt_text =
            docked.size() == 1
                ? fmt::format("Insert filament into {}, then tap Continue", docked_phrase())
                : fmt::format("Insert filament into {}, as far as it goes, then tap "
                              "Continue",
                              docked_phrase());
    } else if (docked.empty()) {
        req.prompt_text =
            on_carriage.size() == 1
                ? fmt::format("Insert filament into Tool {}, then tap Continue",
                              on_carriage.front())
                : fmt::format("Insert filament into Tools {} as far as it goes, then tap Continue",
                              numbers(on_carriage));
    } else {
        req.prompt_text =
            fmt::format("Insert filament into Tool{} {} as far as it goes, and into "
                        "{}, then tap Continue",
                        on_carriage.size() == 1 ? "" : "s", numbers(on_carriage), docked_phrase());
    }
    req.continue_label = "Continue";
    req.cancel_label = "Cancel";
    return req;
}

AmsError AmsBackendToolChanger::start_mount_feed(const std::vector<int>& heads, bool load) {
    namespace mf = helix::mount_feed;
    if (feed_run_) {
        return AmsErrorHelper::busy(load ? "Loading" : "Unloading");
    }
    if (heads.empty()) {
        return AmsErrorHelper::invalid_parameter("mount-to-feed with no head");
    }
    // Ahead of this run's own heating, so Klipper turns an earlier run's
    // heaters off before this one sets any.
    flush_owed_heaters_off();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sensor_error_) {
            // Restoring the carriage needs to know what was on it.
            return AmsError(AmsResult::WRONG_STATE, "Mount-to-feed refused: dock sensor fault",
                            lv_tr("Dock sensors cannot tell which tool is mounted"), "");
        }
    }

    mf::Request req = build_feed_request(heads, load);
    spdlog::info("[AMS ToolChanger] {} {} tool(s) through the carriage (restore='{}')",
                 load ? "Loading" : "Unloading", heads.size(), req.restore_gcode);
    feed_run_.emplace(std::move(req));
    ++feed_generation_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        feed_active_ = true;
        feed_action_ = load ? AmsAction::LOADING : AmsAction::UNLOADING;
        // A fault flagged with no run to stop (another path applies readings
        // too) must not stop this one on its first frame.
        feed_sensor_fault_ = false;
    }
    send_feed_stage();
    return AmsErrorHelper::success();
}

void AmsBackendToolChanger::publish_feed_state_locked() {
    system_info_.action = feed_action_;
    // The status line shows operation_detail ahead of the action, and a frame
    // writes the controller's own phase word there ("idle" between the run's
    // swaps). The run has no detail of its own: the step under way says it.
    system_info_.operation_detail.clear();
    system_info_.operation_phase = feed_phase_;
    step_action_ = feed_step_action_;
}

void AmsBackendToolChanger::send_feed_stage() {
    namespace mf = helix::mount_feed;
    if (!feed_run_) {
        return;
    }
    set_wait_watchdog(false);
    const std::optional<std::string> gcode = feed_run_->current_gcode();
    if (!gcode) {
        if (feed_run_->awaiting_operator()) {
            // The prompt is up: the run moves again on the operator's reply,
            // or on its timeout.
            set_wait_watchdog(true);
            emit_event(EVENT_STATE_CHANGED);
            return;
        }
        spdlog::info("[AMS ToolChanger] Mount-to-feed run {}",
                     feed_run_->aborted() ? "cleaned up" : "complete");
        if (!feed_run_->aborted()) {
            record_fed_heads(*feed_run_);
        }
        if (!feed_run_->aborted() && !feed_run_->request().load) {
            toast_unload_complete();
        }
        reset_feed_run();
        return;
    }

    // A print holds the printer: no swap, move or heater target of this run's
    // may reach it, the cleanup's included.
    if (!refuse_motion_if_printing().success()) {
        stop_feed_run_for_print();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        feed_phase_ = feed_run_->phase_index();
        feed_step_action_ = mf::step_action(feed_run_->current_step());
        const mf::Stage* stage = feed_run_->current_stage();
        feed_swap_in_flight_ = stage != nullptr && stage->swaps;
        feed_prompt_up_ = stage != nullptr && stage->awaits_operator;
        publish_feed_state_locked();
    }
    emit_event(EVENT_STATE_CHANGED);

    const uint64_t generation = feed_generation_;
    auto token = lifetime_.token();
    // Each stage ends on its own ack. The prompt stage acks once the prompt is
    // up; the wait for the operator is not a stage.
    AmsError sent = ensure_homed_then(
        *gcode, [this, generation]() { on_feed_stage_acked(generation); },
        [this, token, generation](const MoonrakerError& err) {
            const bool lost = err.type == MoonrakerErrorType::CONNECTION_LOST;
            token.defer("AmsBackendToolChanger::feed_stage_failed",
                        [this, generation, message = err.message, lost]() {
                            on_feed_stage_failed(generation, message, lost);
                        });
        },
        IMoonrakerAPI::kAmsOperationTimeoutMs, /*skip_homing=*/false, /*silent=*/true,
        /*caller_surfaces_errors=*/false, /*on_predispatch_error=*/nullptr,
        // Every stage waits on its answer, and a swap leaves idle_timeout
        // reading "Printing": the API's busy gate would send a heater-only
        // stage fire-and-forget and drop that answer.
        /*bypass_busy_gate=*/true);
    if (!sent.success()) {
        on_feed_stage_failed(generation, sent.technical_msg);
    }
}

void AmsBackendToolChanger::on_feed_stage_acked(uint64_t generation) {
    if (!feed_run_ || generation != feed_generation_) {
        return;
    }
    if (const std::optional<int> expected = swap_to_confirm()) {
        bool confirmed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            confirmed = swap_confirmed_locked(*expected);
        }
        if (!confirmed) {
            // The ack can lead the frame that reports the finished swap.
            feed_swap_check_ = SwapCheck{generation, *expected};
            set_swap_watchdog(true);
            return;
        }
    }
    advance_feed_run();
}

void AmsBackendToolChanger::advance_feed_run() {
    feed_swap_check_.reset();
    set_swap_watchdog(false);
    feed_run_->stage_done();
    ++feed_generation_;
    // A Cancel held for this swap takes effect now it has settled. A run with
    // nothing left (the restore was the swap) simply completes.
    if (std::exchange(feed_cancel_after_swap_, false) && !feed_run_->aborted() &&
        !feed_run_->finished()) {
        spdlog::info("[AMS ToolChanger] The swap under Cancel settled; unwinding the run");
        toast_partial_run(FeedStopReason::CancelledAfterSwap, feed_run_->completed_heads(),
                          /*carriage_restored=*/false);
        feed_run_->abort(/*restore_carriage=*/false);
    }
    send_feed_stage();
}

std::optional<int> AmsBackendToolChanger::swap_to_confirm() const {
    namespace mf = helix::mount_feed;
    if (!feed_run_ || feed_run_->aborted() || !tool_sensor_.present) {
        return std::nullopt;
    }
    const mf::Stage* stage = feed_run_->current_stage();
    if (stage == nullptr || !stage->swaps) {
        return std::nullopt;
    }
    if (stage->step == mf::Step::Restore) {
        return feed_run_->request().restore_head;
    }
    const auto& heads = feed_run_->request().heads;
    if (stage->tool_index < 0 || static_cast<size_t>(stage->tool_index) >= heads.size()) {
        return std::nullopt;
    }
    return heads[static_cast<size_t>(stage->tool_index)].head;
}

bool AmsBackendToolChanger::swap_confirmed_locked(int expected_slot) const {
    if (raw_sensor_fault_locked() || !sensor_tool_reading_) {
        return false;
    }
    return slot_for_tool_locked(*sensor_tool_reading_) == expected_slot;
}

void AmsBackendToolChanger::recheck_swap() {
    if (!feed_swap_check_ || !feed_run_ || feed_swap_check_->generation != feed_generation_) {
        feed_swap_check_.reset();
        set_swap_watchdog(false);
        return;
    }
    bool confirmed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        confirmed = swap_confirmed_locked(feed_swap_check_->expected_slot);
    }
    if (confirmed) {
        advance_feed_run();
    }
}

void AmsBackendToolChanger::handle_swap_check_elapsed() {
    if (!feed_swap_check_) {
        return;
    }
    spdlog::warn("[AMS ToolChanger] Dock sensors never confirmed the swap to slot {}",
                 feed_swap_check_->expected_slot);
    on_feed_sensor_fault();
}

bool AmsBackendToolChanger::carriage_trusted_for_restore() const {
    if (!tool_sensor_.present) {
        return true;
    }
    if (feed_swap_check_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return !feed_swap_in_flight_ && !raw_sensor_fault_locked();
}

void AmsBackendToolChanger::on_feed_sensor_fault() {
    feed_swap_check_.reset();
    set_swap_watchdog(false);
    if (!feed_run_ || feed_run_->aborted()) {
        return;
    }
    spdlog::warn("[AMS ToolChanger] Mount-to-feed run stopped: the dock sensors cannot vouch for "
                 "the carriage, so it is left where it is");
    toast_partial_run(FeedStopReason::SensorFault, feed_run_->completed_heads(),
                      /*carriage_restored=*/false);
    feed_run_->abort(/*restore_carriage=*/false);
    ++feed_generation_;
    send_feed_stage();
}

void AmsBackendToolChanger::set_swap_watchdog(bool armed) {
    if (!armed) {
        feed_swap_timer_.reset();
        return;
    }
    if (!lv_is_initialized()) {
        return;
    }
    feed_swap_timer_.reset(lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<AmsBackendToolChanger*>(lv_timer_get_user_data(timer))
                ->handle_swap_check_elapsed();
        },
        kSwapConfirmGraceMs, this));
}

void AmsBackendToolChanger::on_feed_stage_failed(uint64_t generation, const std::string& message,
                                                 bool connection_lost) {
    if (!feed_run_ || generation != feed_generation_) {
        return;
    }
    if (connection_lost) {
        // Dropped with the connection, not refused: a cleanup sent now would
        // be dropped the same way.
        spdlog::warn("[AMS ToolChanger] Mount-to-feed stage lost with the connection ({})",
                     message);
        stop_feed_run_unreachable();
        return;
    }
    if (feed_run_->aborted()) {
        // The CLEANUP itself failed to leave: no repeat of the run's toast -
        // the run already accounted for itself. What is new is that a nozzle
        // may still be hot, and nothing further this side can do about it. A
        // cleanup that only closes the prompt heated nothing, so its failure
        // adds nothing to tell.
        const helix::mount_feed::Stage* cleanup = feed_run_->current_stage();
        if (cleanup != nullptr && cleanup->heaters_off) {
            spdlog::error("[AMS ToolChanger] Mount-to-feed cleanup failed to send ({})", message);
            show_feed_toast(lv_tr("Could not turn the heaters off"), ToastSeverity::ERROR);
        } else {
            spdlog::warn("[AMS ToolChanger] Mount-to-feed prompt close failed ({})", message);
        }
        feed_run_->abort(); // already unwinding: this only ends the run
        ++feed_generation_;
        send_feed_stage();
        return;
    }
    // Klipper's own `!!` line reports the failure; this only unwinds. A
    // failure inside the cleanup ends the run where it stands.
    spdlog::warn("[AMS ToolChanger] Mount-to-feed stage failed ({}); cleaning up", message);
    {
        // The error a failed swap stage carries is the controller's own
        // last_error, which the status frame then repeats: this toast tells it.
        std::lock_guard<std::mutex> lock(mutex_);
        controller_failure_noticed_ = message;
    }
    const bool restore = carriage_trusted_for_restore();
    toast_partial_run(FeedStopReason::Failed, feed_run_->completed_heads(), restore);
    feed_run_->abort(restore);
    ++feed_generation_;
    send_feed_stage();
}

void AmsBackendToolChanger::on_feed_reply(helix::mount_feed::Reply reply) {
    if (reply == helix::mount_feed::Reply::Continue) {
        on_feed_signal(helix::mount_feed::ConsoleSignal::Continue);
    } else if (reply == helix::mount_feed::Reply::Cancel) {
        on_feed_signal(helix::mount_feed::ConsoleSignal::Cancel);
    }
}

void AmsBackendToolChanger::on_feed_signal(helix::mount_feed::ConsoleSignal signal) {
    if (!feed_run_ || !feed_run_->awaiting_operator()) {
        return;
    }
    if (signal == helix::mount_feed::ConsoleSignal::Continue) {
        spdlog::info("[AMS ToolChanger] Operator confirmed the filament feed");
        feed_run_->operator_reply(helix::mount_feed::Reply::Continue);
    } else {
        // Cancel, and a dismissal: once the prompt is gone from the console
        // the operator can no longer answer it, so both unwind the run.
        spdlog::info("[AMS ToolChanger] Operator {} the filament feed",
                     signal == helix::mount_feed::ConsoleSignal::Dismissed ? "dismissed"
                                                                           : "cancelled");
        toast_partial_run(FeedStopReason::Cancelled, feed_run_->completed_heads());
        feed_run_->operator_reply(helix::mount_feed::Reply::Cancel);
    }
    ++feed_generation_;
    send_feed_stage();
}

void AmsBackendToolChanger::handle_wait_elapsed(uint32_t elapsed_ms) {
    if (!feed_run_) {
        return;
    }
    // The list is read before the wait aborts the run: abort() collapses the
    // stage list the answer is derived from.
    const std::vector<int> completed = feed_run_->completed_heads();
    if (feed_run_->advance_wait(elapsed_ms) != helix::mount_feed::Run::WaitResult::Expired) {
        return;
    }
    spdlog::warn("[AMS ToolChanger] Operator never inserted the filament; cleaning up");
    toast_partial_run(FeedStopReason::TimedOut, completed);
    ++feed_generation_;
    send_feed_stage();
}

void AmsBackendToolChanger::toast_partial_run(FeedStopReason why, const std::vector<int>& completed,
                                              bool carriage_restored,
                                              const std::vector<int>& left_heating) {
    if (!feed_run_) {
        return;
    }
    const auto& req = feed_run_->request();
    const bool several = req.heads.size() > 1;
    // The carriage is left wherever the stop found it, mid-swap perhaps, when
    // the run moved anything and had a restore planned that is not going out.
    const bool carriage_left = !carriage_restored && feed_run_->started() &&
                               !req.restore_gcode.empty() && why != FeedStopReason::SensorFault &&
                               why != FeedStopReason::CancelledAfterSwap;
    if (why == FeedStopReason::Cancelled && !several && !carriage_left) {
        // A single tool's Cancel is the operator's own act, said on the prompt
        // they tapped; only a multi-tool run stops with accounting due.
        return;
    }
    std::vector<std::string> sentences;
    if (why == FeedStopReason::TimedOut) {
        // The prompt is the run's first stage: nothing has heated or moved.
        sentences.emplace_back(lv_tr("No filament was inserted in time"));
    } else if (several) {
        sentences.emplace_back(req.load ? lv_tr("Load all stopped") : lv_tr("Unload all stopped"));
    } else {
        // One tool never was an "all": the toast is about its own load.
        sentences.emplace_back(req.load ? lv_tr("Load stopped") : lv_tr("Unload stopped"));
    }
    if (why == FeedStopReason::SensorFault) {
        sentences.emplace_back(lv_tr("Dock sensors cannot tell which tool is mounted"));
    } else if (why == FeedStopReason::Unreachable) {
        sentences.emplace_back(lv_tr("The connection to Klipper was lost"));
    } else if (why == FeedStopReason::PrintStarted) {
        sentences.emplace_back(lv_tr("A print started"));
    } else if (why == FeedStopReason::CancelledAfterSwap) {
        // The sensors confirmed the swap, so which tool is mounted is known.
        sentences.emplace_back(
            req.restore_head >= 0
                ? fmt::format(fmt::runtime(lv_tr("The tool change under way finished, and {} "
                                                 "was not put back on the carriage")),
                              helix::ui::lane_label(lane_noun(), req.restore_head))
                : std::string(lv_tr("The tool change under way finished, and the tool was not "
                                    "parked")));
    }
    if (carriage_left) {
        sentences.emplace_back(lv_tr("The carriage was not put back: check which tool is mounted"));
    }
    if (const std::string heating = still_heating_sentence(left_heating); !heating.empty()) {
        sentences.push_back(heating);
    }
    const std::string list = feed_lane_list(completed);
    if (!list.empty()) {
        sentences.push_back(fmt::format(fmt::runtime(lv_tr("Completed: {}")), list));
    }
    std::string text;
    for (const std::string& sentence : sentences) {
        text += (text.empty() ? "" : " ") + sentence + ".";
    }
    // A Cancel the run carried out as asked is no error.
    show_feed_toast(text, why == FeedStopReason::CancelledAfterSwap ? ToastSeverity::WARNING
                                                                    : ToastSeverity::ERROR);
}

std::string AmsBackendToolChanger::still_heating_sentence(const std::vector<int>& heads) const {
    if (heads.empty()) {
        return {};
    }
    if (heads.size() == 1) {
        return fmt::format(fmt::runtime(lv_tr("{}'s nozzle is still heating")),
                           feed_lane_list(heads));
    }
    return fmt::format(fmt::runtime(lv_tr("Nozzles still heating: {}")), feed_lane_list(heads));
}

void AmsBackendToolChanger::toast_unload_complete() {
    if (!feed_run_) {
        return;
    }
    std::vector<int> heads;
    for (const auto& head : feed_run_->request().heads) {
        heads.push_back(head.head);
    }
    show_feed_toast(
        fmt::format(fmt::runtime(lv_tr("Unload complete. Pull the filament out of {}.")),
                    feed_lane_list(heads)),
        ToastSeverity::SUCCESS);
}

std::string AmsBackendToolChanger::feed_lane_list(const std::vector<int>& heads) const {
    std::string list;
    for (int head : heads) {
        if (!list.empty()) {
            list += ", ";
        }
        list += helix::ui::lane_label(lane_noun(), head);
    }
    return list;
}

void AmsBackendToolChanger::show_feed_toast(const std::string& message, ToastSeverity severity) {
    // Recorded before the guard: unit tests pin the wording without a display.
    feed_last_toast_ = message;
    feed_last_toast_severity_ = severity;
    if (!lv_is_initialized()) {
        return;
    }
    ToastManager::instance().show(severity, message.c_str());
}

void AmsBackendToolChanger::abandon_feed_run_after_disconnect() {
    if (!feed_run_) {
        return;
    }
    // No ack can arrive for the stage in flight, and the operator prompt
    // stays up on every other client. Send the whole cleanup best effort and
    // reset the run now, rather than leaving a hot nozzle behind bookkeeping.
    spdlog::warn("[AMS ToolChanger] Backend stopping under a mount-to-feed run; sending the "
                 "cleanup and resetting the run");
    feed_run_->abort(carriage_trusted_for_restore());
    const std::optional<std::string> cleanup = feed_run_->current_gcode();
    reset_feed_run();
    if (cleanup) {
        execute_gcode(*cleanup);
    }
}

void AmsBackendToolChanger::stop_feed_run_unreachable() {
    if (!feed_run_) {
        return;
    }
    spdlog::warn("[AMS ToolChanger] The printer cannot take the mount-to-feed run's gcode; "
                 "ending the run, its heaters-off owed until Klippy is ready");
    if (!feed_run_->aborted()) {
        toast_partial_run(FeedStopReason::Unreachable, feed_run_->completed_heads(),
                          /*carriage_restored=*/false);
    }
    owe_heaters_off(feed_run_->heater_heads());
    reset_feed_run();
}

void AmsBackendToolChanger::stop_feed_run_for_print() {
    if (!feed_run_) {
        return;
    }
    spdlog::warn("[AMS ToolChanger] A print holds the printer; ending the mount-to-feed run "
                 "without its cleanup");
    const helix::mount_feed::Stage* stage = feed_run_->current_stage();
    const bool prompt_open = stage != nullptr && stage->closes_prompt;
    // Nothing goes out to turn them off: the print may be using them. The
    // operator is told which, so they can turn the others off themselves.
    const std::vector<int> heating = feed_run_->heads_left_heating();
    if (!feed_run_->aborted()) {
        toast_partial_run(FeedStopReason::PrintStarted, feed_run_->completed_heads(),
                          /*carriage_restored=*/false, heating);
    } else if (const std::string sentence = still_heating_sentence(heating); !sentence.empty()) {
        // The run already said why it stopped; what is new is the heat.
        show_feed_toast(sentence + ".", ToastSeverity::WARNING);
    }
    reset_feed_run();
    if (prompt_open) {
        // The operator answered, but the dialog stays up on every client until
        // it is closed; a RESPOND line moves nothing.
        execute_gcode(helix::mount_feed::prompt_end_gcode());
    }
}

void AmsBackendToolChanger::flush_owed_heaters_off() {
    if (owed_heaters_off_.empty()) {
        return;
    }
    if (!frame_print_state_current_) {
        // Not yet known whether the reconnect found a print running: sending
        // now could zero a new job's heaters.
        return;
    }
    // A print owns the heaters it finds: zeroing one now would cool a nozzle
    // the job is using. What the run owed is the print's to set from here, so
    // the operator is told which were left set.
    const bool print_holds = frame_print_state_.empty() ? print_holds_printer()
                                                        : frame_print_state_ == "printing" ||
                                                              frame_print_state_ == "paused";
    if (print_holds) {
        spdlog::info("[AMS ToolChanger] A print holds the printer; dropping the heaters-off a "
                     "stopped mount-to-feed run owed");
        const std::vector<int> heads = std::exchange(owed_heaters_off_, {});
        show_feed_toast(still_heating_sentence(heads) + ".", ToastSeverity::WARNING);
        return;
    }
    std::vector<int> heads = std::exchange(owed_heaters_off_, {});
    spdlog::info("[AMS ToolChanger] Turning off {} heater(s) a stopped mount-to-feed run left on",
                 heads.size());
    auto token = lifetime_.token();
    const AmsError sent = execute_gcode(
        helix::mount_feed::heaters_off_gcode(heads), nullptr,
        [this, token, heads](const MoonrakerError& err) {
            if (err.type != MoonrakerErrorType::CONNECTION_LOST) {
                spdlog::error("[AMS ToolChanger] Heaters-off after a stopped run refused: {}",
                              err.message);
                return;
            }
            // Dropped again: owed until the printer is back, beside whatever
            // another stopped run came to owe meanwhile.
            token.defer("AmsBackendToolChanger::owed_heaters_off_lost",
                        [this, heads]() { owe_heaters_off(heads); });
        },
        /*silent=*/true);
    if (!sent.success()) {
        owe_heaters_off(heads);
    }
}

void AmsBackendToolChanger::owe_heaters_off(const std::vector<int>& heads) {
    for (int head : heads) {
        if (std::find(owed_heaters_off_.begin(), owed_heaters_off_.end(), head) ==
            owed_heaters_off_.end()) {
            owed_heaters_off_.push_back(head);
        }
    }
}

void AmsBackendToolChanger::reset_feed_run() {
    feed_run_.reset();
    feed_swap_check_.reset();
    feed_cancel_after_swap_ = false;
    set_swap_watchdog(false);
    set_wait_watchdog(false);
    ++feed_generation_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        feed_active_ = false;
        feed_swap_in_flight_ = false;
        feed_prompt_up_ = false;
        feed_sensor_fault_ = false;
        feed_phase_ = -1;
        feed_step_action_.reset();
        system_info_.action = AmsAction::IDLE;
        system_info_.operation_phase = -1;
        step_action_.reset();
        // The run held the published state through a sensor fault; once it is
        // over, a fault the sensors still report is what shows.
        if (tool_sensor_.present && raw_sensor_fault_locked()) {
            sensor_error_ = true;
            system_info_.action = AmsAction::ERROR;
            system_info_.operation_detail = "sensor error";
        }
    }
    emit_event(EVENT_STATE_CHANGED);
}

void AmsBackendToolChanger::set_wait_watchdog(bool armed) {
    if (!armed) {
        feed_wait_timer_.reset();
        return;
    }
    feed_wait_last_tick_ = feed_clock_();
    if (!lv_is_initialized()) {
        return;
    }
    feed_wait_timer_.reset(lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<AmsBackendToolChanger*>(lv_timer_get_user_data(timer))->feed_wait_tick();
        },
        /*period_ms=*/1000, this));
}

void AmsBackendToolChanger::feed_wait_tick() {
    if (!feed_run_ || !feed_run_->awaiting_operator()) {
        feed_wait_timer_.reset();
        return;
    }
    // The run sums what it is handed, so each tick passes only its own share.
    const auto now = feed_clock_();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - feed_wait_last_tick_);
    feed_wait_last_tick_ = now;
    handle_wait_elapsed(static_cast<uint32_t>(std::max<int64_t>(elapsed.count(), 0)));
}

void AmsBackendToolChanger::register_mount_feed_listener() {
    if (!client_ || !mounts_to_feed_) {
        return;
    }
    static const std::string handler_name = "toolchanger_mount_feed_watcher";
    auto token = lifetime_.token();
    client_->register_method_callback(
        "notify_gcode_response", handler_name, [this, token](const json& msg) {
            // BG THREAD: extract the line into a local; no `this` access here.
            std::string line;
            if (msg.contains("params") && msg["params"].is_array() && !msg["params"].empty() &&
                msg["params"][0].is_string()) {
                line = msg["params"][0].get<std::string>();
            } else if (msg.is_array() && !msg.empty() && msg[0].is_string()) {
                line = msg[0].get<std::string>();
            }
            if (!helix::mount_feed::may_be_prompt_signal(line)) {
                return;
            }
            const auto signal = helix::mount_feed::parse_console(line);
            if (signal == helix::mount_feed::ConsoleSignal::None) {
                return;
            }
            token.defer("AmsBackendToolChanger::feed_reply",
                        [this, signal]() { on_feed_signal(signal); });
        });
}

void AmsBackendToolChanger::unregister_mount_feed_listener() {
    if (!client_) {
        return;
    }
    client_->unregister_method_callback("notify_gcode_response", "toolchanger_mount_feed_watcher");
}

HeadFilamentOps AmsBackendToolChanger::head_ops_for(bool feeds_head, bool cut_to_unload) {
    HeadFilamentOps ops;
    const char* by_hand = lv_tr("Load and unload filament at the tool by hand");
    if (!feeds_head) {
        ops.load = FilamentOpSupport::Manual;
        ops.load_reason = by_hand;
    }
    ops.unload = FilamentOpSupport::Manual;
    ops.unload_reason = cut_to_unload ? lv_tr("This printer has no automatic unload. Cut the "
                                              "filament at the head and pull it out by hand.")
                                      : by_hand;
    return ops;
}

HeadFilamentOps AmsBackendToolChanger::head_filament_ops(int head) const {
    if (mounts_to_feed_) {
        return {}; // Automatic both ways: the op mounts the tool itself
    }
    const bool feeds = tool_feed_.present && head >= 0 &&
                       static_cast<size_t>(head) < tool_feed_.head_gcode.size() &&
                       !tool_feed_.head_gcode[static_cast<size_t>(head)].empty();
    return head_ops_for(feeds, unload_is_manual());
}

AmsError AmsBackendToolChanger::do_load_filament(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        AmsError slot_valid = validate_slot_index_locked(slot_index);
        if (!slot_valid) {
            return slot_valid;
        }
    }
    if (mounts_to_feed_) {
        return start_mount_feed({slot_index}, /*load=*/true);
    }
    // Only a feed the printer published is sent. A load never mounts a tool:
    // the toolhead tap does that.
    const auto head = static_cast<size_t>(slot_index);
    if (!tool_feed_.present || head >= tool_feed_.head_gcode.size() ||
        tool_feed_.head_gcode[head].empty()) {
        return AmsError(AmsResult::NOT_SUPPORTED, "No filament feed for this tool",
                        head_filament_ops(slot_index).load_reason, "");
    }
    // The firmware feeds the head itself, and heats it.
    std::string cmd = tool_feed_.head_gcode[head];
    spdlog::info("[AMS ToolChanger] Feeding tool {} via {}: {}", slot_index,
                 tool_feed_.provider_name, cmd);
    return dispatch_operation(std::move(cmd), AmsAction::LOADING);
}

AmsError AmsBackendToolChanger::do_unload_filament(int slot_index) {
    if (mounts_to_feed_) {
        int head = slot_index;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (head < 0) {
                head = system_info_.current_slot;
            }
            if (head < 0) {
                return AmsErrorHelper::not_loaded();
            }
            AmsError slot_valid = validate_slot_index_locked(head);
            if (!slot_valid) {
                return slot_valid;
            }
        }
        return start_mount_feed({head}, /*load=*/false);
    }
    // Without a feed nothing reverses on a tool changer: taking filament out is
    // the user's job, and an unload never unmounts a tool (park_active_tool() does).
    return AmsError(AmsResult::NOT_SUPPORTED, "No filament unload on this tool changer",
                    head_filament_ops(std::max(slot_index, 0)).unload_reason, "");
}

AmsError AmsBackendToolChanger::do_filament_batch(const std::vector<int>& slots, bool load) {
    if (!mounts_to_feed_) {
        return AmsErrorHelper::not_supported("Batch filament ops");
    }
    if (slots.empty()) {
        return AmsErrorHelper::invalid_parameter("batch filament op with no slots");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int slot : slots) {
            AmsError slot_valid = validate_slot_index_locked(slot);
            if (!slot_valid) {
                return slot_valid;
            }
        }
    }
    return start_mount_feed(slots, load);
}

AmsError AmsBackendToolChanger::do_park_active_tool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sensor_error_) {
            return sensor_fault_refusal("Unmount");
        }
        if (system_info_.current_tool < 0) {
            return AmsErrorHelper::not_loaded();
        }
    }

    std::string cmd = park_gcode();
    if (cmd.empty()) {
        return AmsErrorHelper::not_supported("unmount");
    }
    // UNSELECT_TOOL has the same no-op shortcut as SELECT_TOOL — unmounting when
    // nothing is on the carriage returns without touching toolchanger.status —
    // so it needs the same optimistic-set + ack-resolution treatment (#1183).
    spdlog::info("[AMS ToolChanger] Parking the current tool: {}", cmd);
    return dispatch_operation(std::move(cmd), AmsAction::UNLOADING);
}

AmsError AmsBackendToolChanger::do_select_slot(int slot_index) {
    // For tool changers, selecting a slot means mounting that tool
    return do_change_tool(slot_index);
}

AmsError AmsBackendToolChanger::do_change_tool(int tool_number) {
    std::string cmd;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError slot_valid = validate_slot_index_locked(tool_number);
        if (!slot_valid) {
            return slot_valid;
        }
        // A swap plans its moves from what is on the carriage.
        if (sensor_error_) {
            return sensor_fault_refusal("Tool change");
        }
        cmd = mount_gcode(tool_number);
    }

    // mount_gcode() names the physical tool, so the swap mounts the tool the
    // user tapped whatever the slicer's T<n> was assigned to.
    //
    // dispatch_operation() sets SELECTING before the send — which also closes
    // the race window where a second tap could arrive before Klipper's status
    // update changes the action — and resolves it on the macro's ack when the
    // toolchanger never claims the operation (#1183).
    // Without klipper-toolchanger there is no SELECT_TOOL: the extra registers
    // its own T<n> commands and those ARE the swap. ASSIGN_TOOL does not exist
    // on such a machine either, so the remap concern above cannot arise.
    spdlog::info("[AMS ToolChanger] Mounting tool {}: {}", tool_number, cmd);
    return dispatch_operation(std::move(cmd), AmsAction::SELECTING);
}

std::string AmsBackendToolChanger::mount_gcode(int slot_index) const {
    if (tool_commands_.present) {
        return tool_commands_.select_prefix + std::to_string(slot_index);
    }
    // TOOL= names the [tool ...] object, a physical tool. T= is a tool NUMBER,
    // which ASSIGN_TOOL can point at any other hot end.
    if (slot_index >= 0 && slot_index < static_cast<int>(tool_names_.size())) {
        return "SELECT_TOOL TOOL=" + tool_object_arg(tool_names_[static_cast<size_t>(slot_index)]);
    }
    return "SELECT_TOOL T=" + std::to_string(slot_index);
}

std::string AmsBackendToolChanger::tool_object_arg(const std::string& tool_name) {
    // klipper-toolchanger looks a TOOL= argument up by the tool's full object
    // name, which has a space in it; Klipper reads quoted parameter values.
    return fmt::format("\"tool {}\"", tool_name);
}

AmsError AmsBackendToolChanger::sensor_fault_refusal(const char* operation) {
    return AmsError(AmsResult::WRONG_STATE,
                    std::string(operation) + " refused: dock sensors cannot identify the tool",
                    lv_tr("Dock sensors cannot tell which tool is mounted"), "");
}

std::string AmsBackendToolChanger::park_gcode() const {
    // A changer whose own extra does the swapping has no UNSELECT_TOOL; its
    // unmount takes no tool argument, because there is only ever one tool on
    // the head to drop. Empty when it has none.
    return tool_commands_.present ? tool_commands_.unselect : std::string("UNSELECT_TOOL");
}

// ============================================================================
// Recovery Operations
// ============================================================================

bool AmsBackendToolChanger::reset_available() const {
    // INITIALIZE_TOOLCHANGER is klipper-toolchanger's; a changer driving its
    // own T<n> without it has nothing to reinitialize from here.
    return !tool_commands_.present || tool_commands_.initialize_available;
}

bool AmsBackendToolChanger::recover_available() const {
    return reset_available();
}

AmsError AmsBackendToolChanger::recover() {
    if (!recover_available()) {
        return AmsErrorHelper::not_supported("Recover");
    }
    spdlog::info("[AMS ToolChanger] Attempting recovery");
    // klipper-toolchanger doesn't have a dedicated recovery command
    // Try to reinitialize the toolchanger
    return execute_gcode("INITIALIZE_TOOLCHANGER");
}

AmsError AmsBackendToolChanger::reset() {
    if (!reset_available()) {
        return AmsErrorHelper::not_supported("Reset");
    }
    spdlog::info("[AMS ToolChanger] Resetting toolchanger");
    return execute_gcode("INITIALIZE_TOOLCHANGER");
}

void AmsBackendToolChanger::on_klipper_restarted() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_dispatch_action_.reset();
        ++dispatch_generation_;
        // Whatever the controller was doing died with it.
        last_operation_word_.clear();
    }
    // Everything the run had queued died with the restart. Klippy is ready
    // again, so what the run owes its heaters can go out now.
    stop_feed_run_unreachable();
    flush_owed_heaters_off();
    AmsSubscriptionBackend::on_klipper_restarted();
}

void AmsBackendToolChanger::on_connection_lost() {
    {
        // A phase word from before the drop says nothing about the printer
        // the reconnect finds.
        std::lock_guard<std::mutex> lock(mutex_);
        last_operation_word_.clear();
    }
    frame_print_state_current_ = false;
    stop_feed_run_unreachable();
}

AmsError AmsBackendToolChanger::cancel() {
    if (feed_run_ && feed_cancel_after_swap_) {
        // Already asked: the run ends once the swap settles.
        return AmsErrorHelper::success();
    }
    if (can_cancel_operation()) {
        // Only the dock sensors make a swap something to wait out: without
        // them the cleanup queues behind it in Klipper and restores as usual.
        bool swap_under_way = tool_sensor_.present && feed_swap_check_.has_value();
        if (tool_sensor_.present) {
            std::lock_guard<std::mutex> lock(mutex_);
            swap_under_way = swap_under_way || feed_swap_in_flight_;
        }
        if (swap_under_way) {
            // Klipper finishes the swap whatever this side does, and a cleanup
            // sent now would end the run while the carriage still moves. The
            // run stays busy until the swap settles, then unwinds.
            spdlog::info("[AMS ToolChanger] Cancel: ending the mount-to-feed run once the swap "
                         "under way settles");
            feed_cancel_after_swap_ = true;
            emit_event(EVENT_STATE_CHANGED);
            return AmsErrorHelper::success();
        }
        spdlog::info("[AMS ToolChanger] Cancel: unwinding the mount-to-feed run");
        const bool restore = carriage_trusted_for_restore();
        toast_partial_run(FeedStopReason::Cancelled, feed_run_->completed_heads(), restore);
        feed_run_->abort(restore);
        // An ack still in flight belongs to the stage being dropped. Klipper
        // queues the cleanup behind it, so the heater goes off once it is done.
        ++feed_generation_;
        send_feed_stage();
        return AmsErrorHelper::success();
    }
    spdlog::info("[AMS ToolChanger] Cancel requested (not implemented for tool changes)");
    // Tool changes typically can't be cancelled mid-operation
    return AmsErrorHelper::not_supported("Cancel");
}

bool AmsBackendToolChanger::can_cancel_operation() const {
    // Main thread, like every feed_run_ access.
    return feed_run_ && !feed_run_->aborted() && !feed_cancel_after_swap_;
}

// ============================================================================
// Configuration Operations
// ============================================================================

AmsError AmsBackendToolChanger::apply_user_edit(int slot_index, const SlotInfo& info,
                                                const helix::ams::Observation& declared) {
    std::string physical_tool_name;
    bool firmware_edit = false;
    std::vector<std::pair<int, std::uint32_t>> palette;
    std::string (*write_gcode)(int, const std::string&, std::uint32_t) = nullptr;
    std::string firmware_type;
    bool remap_refused = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError slot_valid = validate_slot_index_locked(slot_index);
        if (!slot_valid) {
            return slot_valid;
        }

        // Decide the firmware write from the current slot reading. Only the
        // decision inputs are taken here: the material check below has to run
        // before anything is written, and it cannot run under mutex_.
        if (!system_info_.units.empty() &&
            slot_index < static_cast<int>(system_info_.units[0].slots.size())) {
            const auto& slot = system_info_.units[0].slots[slot_index];
            firmware_edit = material_source_.present && material_source_.write_gcode &&
                            firmware_slots_seen_ && firmware_palette_ &&
                            !firmware_palette_->empty() &&
                            (slot.color_rgb != info.color_rgb || slot.material != info.material);
            if (firmware_edit) {
                palette = *firmware_palette_;
                write_gcode = material_source_.write_gcode;
            }
        }
    }

    if (firmware_edit) {
        // Outside mutex_: normalize_material() reads get_supported_materials(),
        // which locks. The firmware stores a type from its own list, so the
        // material is mapped before the send. A type that cannot be sent is
        // refused here, ahead of every write below: a refused edit leaves the
        // slot fields, the staged override, the persisted record and the
        // slot-changed event untouched.
        firmware_type = normalize_material(info.material);
        if (!IMoonrakerAPI::is_safe_material_param(firmware_type)) {
            return AmsErrorHelper::invalid_parameter("Material '" + firmware_type +
                                                     "' cannot be stored on this printer");
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Update local state (for UI display)
        if (!system_info_.units.empty() &&
            slot_index < static_cast<int>(system_info_.units[0].slots.size())) {
            auto& slot = system_info_.units[0].slots[slot_index];
            const int old_mapped_tool = slot.mapped_tool;
            slot.assign_filament_fields(info);

            // Tool mapping change: persist via ASSIGN_TOOL outside the lock.
            // slot.mapped_tool stores "which G-code tool number activates this physical
            // tool"; the gcode says "T<info.mapped_tool> now activates tool_names_[slot]".
            if (info.mapped_tool != old_mapped_tool && tool_commands_.present) {
                // No ASSIGN_TOOL on this machine: the filament half of the edit
                // lands and the tool number stays where the firmware has it.
                remap_refused = true;
            } else if (info.mapped_tool != old_mapped_tool && info.mapped_tool >= 0 &&
                       info.mapped_tool < static_cast<int>(system_info_.tool_to_slot_map.size()) &&
                       slot_index < static_cast<int>(tool_names_.size())) {
                // One pass for both directions, and it EVICTS: the tool being
                // moved leaves its old lane unmapped, and whatever tool this
                // lane previously answered to gives it up. Writing the two
                // fields by hand left both stale halves in place, so a swap
                // ended with two lanes claiming one tool number.
                helix::printer::assign_tool_slot(system_info_, info.mapped_tool, slot_index);
                physical_tool_name = tool_names_[slot_index];
            }

            // Stage the user's edit. Every field is override-exclusive here:
            // klipper-toolchanger supplies no material, colour, brand or weight,
            // so there is nothing underneath for these to fall through to.
            helix::ams::stage_user_override(overrides_, slot_index, info, declared);
        }
    }

    // Persist BEFORE the remap's early return, or a slot edit that also moved a
    // tool number would send ASSIGN_TOOL and silently drop the metadata.
    if (override_store_) {
        helix::ams::persist_staged_override(override_store_.get(), mutex_, overrides_, slot_index,
                                            backend_log_tag(), "Override");
    }

    // Emit OUTSIDE the lock to avoid deadlock with callbacks, and ahead of the
    // remap's return so an edit that also moves a tool number is announced too.
    emit_event(kEventSlotChanged, std::to_string(slot_index));

    if (firmware_edit) {
        // The colour is snapped to the firmware's own palette; the type was
        // normalized and validated above, before any write went out.
        const int index = helix::nearest_palette_key(palette, info.color_rgb, -1);
        std::uint32_t rgb = palette.front().second;
        for (const auto& [key, packed] : palette) {
            if (key == index) {
                rgb = packed;
                break;
            }
        }
        AmsError sent = execute_gcode(
            write_gcode(slot_index, IMoonrakerAPI::gcode_param_value(firmware_type), rgb));
        if (!sent.success()) {
            // The slot fields, the staged override, the persisted record and
            // the event have already gone out; only the firmware's copy of
            // colour and material is missing, and its next status frame
            // restates the old values. The edit is still worth declaring.
            sent.partially_applied = true;
            return sent;
        }
    }

    if (remap_refused) {
        AmsError refused = AmsErrorHelper::not_supported("Tool remapping");
        refused.partially_applied = true;
        return refused;
    }

    if (!physical_tool_name.empty()) {
        spdlog::info("[AMS ToolChanger] Remap via slot edit: T{} -> physical {} (slot {})",
                     info.mapped_tool, physical_tool_name, slot_index);
        return assign_tool(physical_tool_name, info.mapped_tool);
    }

    return AmsErrorHelper::success();
}

void AmsBackendToolChanger::clear_slot_override(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_override_locked(slot_index, system_info_.get_slot_global(slot_index));
    }
    emit_event(kEventSlotChanged, std::to_string(slot_index));
}

void AmsBackendToolChanger::clear_override_fields(SlotInfo& slot) const {
    AmsSubscriptionBackend::clear_override_fields(slot);
    slot.material.clear();
    slot.color_rgb = kAmsDefaultSlotColor;
    slot.multi_color_hexes.clear();
}

AmsError AmsBackendToolChanger::sync_external_identity(int slot_index, const SlotInfo& info) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError slot_valid = validate_slot_index_locked(slot_index);
        if (!slot_valid) {
            return slot_valid;
        }

        // The slot keeps the tool number it answers to: only a person's edit moves
        // the tool map, and moving it sends ASSIGN_TOOL.
        if (!system_info_.units.empty() &&
            slot_index < static_cast<int>(system_info_.units[0].slots.size())) {
            system_info_.units[0].slots[slot_index].assign_filament_fields(info);
        }
    }

    // Emit OUTSIDE the lock to avoid deadlock with callbacks
    emit_event(kEventSlotChanged, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

void AmsBackendToolChanger::persist_external_identity_impl(
    int slot_index, const helix::ams::Observation& spoolman) {
    const std::string tag = backend_log_tag();
    std::lock_guard<std::mutex> lock(mutex_);
    helix::ams::persist_override_external_identity(override_store_.get(), overrides_, slot_index,
                                                   spoolman, tag);
}

void AmsBackendToolChanger::persist_slot_weight(int slot_index, float remaining_weight_g,
                                                float total_weight_g) {
    const std::string tag = backend_log_tag();
    std::lock_guard<std::mutex> lock(mutex_);
    helix::ams::persist_override_weight(override_store_.get(), overrides_, slot_index,
                                        remaining_weight_g, total_weight_g, tag);
}

AmsError AmsBackendToolChanger::assign_tool(const std::string& physical_tool_name,
                                            int tool_number) {
    // ASSIGN_TOOL is klipper-toolchanger's. tool_commands_.present means this
    // machine drives swaps with its own T<n> commands instead, and those name
    // the physical tool outright - there is no tool-number indirection to
    // rewrite. Klipper logs an unknown command and carries on, so sending it
    // anyway reads as a successful remap and prints the firmware's own mapping.
    if (tool_commands_.present) {
        spdlog::warn("[AMS ToolChanger] {} has no tool-number remap; T{} -> {} not sent",
                     tool_commands_.provider_name, tool_number, physical_tool_name);
        return AmsErrorHelper::not_supported("Tool remapping");
    }
    return execute_gcode(
        fmt::format("ASSIGN_TOOL TOOL={} N={}", tool_object_arg(physical_tool_name), tool_number));
}

AmsError AmsBackendToolChanger::can_set_tool_mapping(int tool_number, int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return can_set_tool_mapping_locked(tool_number, slot_index);
}

AmsError AmsBackendToolChanger::can_set_tool_mapping_locked(int tool_number, int slot_index) const {
    const int tool_count = static_cast<int>(tool_names_.size());
    if (tool_number < 0 || tool_number >= tool_count) {
        return AmsErrorHelper::tool_out_of_range(tool_number);
    }
    if (slot_index < 0 || slot_index >= tool_count) {
        return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, tool_count - 1);
    }
    return AmsErrorHelper::success();
}

AmsError AmsBackendToolChanger::set_tool_mapping_impl(int tool_number, int slot_index) {
    // Remap G-code tool number to a different physical tool via klipper-toolchanger's
    // ASSIGN_TOOL command. This makes Klipper's T<tool_number> command activate the
    // physical tool at slot_index instead of tool_number.
    //
    // Example: set_tool_mapping(0, 2) sends ASSIGN_TOOL TOOL="tool T2" N=0
    //   → G-code "T0" now activates physical tool T2
    std::string physical_tool_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (auto err = can_set_tool_mapping_locked(tool_number, slot_index); !err.success()) {
            return err;
        }
        // No ASSIGN_TOOL to send, so the table stays as the firmware has it.
        if (tool_commands_.present) {
            return AmsErrorHelper::not_supported("Tool remapping");
        }

        // The physical tool to assign (slot_index maps to tool_names_[slot_index])
        physical_tool_name = tool_names_[slot_index];

        // Update internal mapping — both directions, with eviction. The AMS
        // panel badges a lane from SlotInfo::mapped_tool while the filament
        // panel's op buttons resolve their lane through tool_to_slot_map;
        // writing only the forward entry here left the badge on the lane the
        // tool had just been moved away from.
        helix::printer::assign_tool_slot(system_info_, tool_number, slot_index);
    }

    spdlog::info("[AMS ToolChanger] Remapping T{} -> physical {} (slot {})", tool_number,
                 physical_tool_name, slot_index);
    return assign_tool(physical_tool_name, tool_number);
}

std::vector<int> AmsBackendToolChanger::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.tool_to_slot_map;
}

AmsError AmsBackendToolChanger::reset_tool_mappings() {
    // Restore identity mapping: each tool number maps to its own physical tool.
    // Sends ASSIGN_TOOL for each tool that was remapped away from identity.
    std::vector<std::pair<int, std::string>> remaps_needed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int tool_count = static_cast<int>(tool_names_.size());
        for (int i = 0; i < tool_count; ++i) {
            if (i < static_cast<int>(system_info_.tool_to_slot_map.size()) &&
                system_info_.tool_to_slot_map[i] != i) {
                // Tool i is currently mapped to a different slot — restore to identity
                remaps_needed.emplace_back(i, tool_names_[i]);
                system_info_.tool_to_slot_map[i] = i;
            }
        }
        // Re-derive every slot's mapped_tool from the identity map just
        // written. The loop above touches only the forward direction, so
        // without this the lane badges kept showing the remap the user just
        // reset — the panel and the op buttons disagreeing again, one frame
        // after we told the user the reset succeeded.
        if (!remaps_needed.empty()) {
            helix::printer::sync_tool_map_from_forward(system_info_,
                                                       /*identity_fallback=*/true);
        }
    }

    if (remaps_needed.empty()) {
        spdlog::debug("[AMS ToolChanger] All tools already at identity mapping");
        return AmsErrorHelper::success();
    }

    AmsError last_error = AmsErrorHelper::success();
    for (const auto& [tool_num, tool_name] : remaps_needed) {
        spdlog::info("[AMS ToolChanger] Resetting T{} -> physical {} (identity)", tool_num,
                     tool_name);
        auto err = assign_tool(tool_name, tool_num);
        if (err.result != AmsResult::SUCCESS) {
            last_error = err;
        }
    }

    spdlog::info("[AMS ToolChanger] Reset {} tool mapping(s) to identity", remaps_needed.size());
    return last_error;
}

// ============================================================================
// Bypass Mode (Not Applicable)
// ============================================================================

AmsError AmsBackendToolChanger::enable_bypass() {
    return AmsErrorHelper::not_supported("Bypass mode");
}

AmsError AmsBackendToolChanger::disable_bypass() {
    return AmsErrorHelper::not_supported("Bypass mode");
}

bool AmsBackendToolChanger::is_bypass_active() const {
    return false; // Tool changers never have bypass
}

// ============================================================================
// Device Actions
// ============================================================================

std::vector<helix::printer::DeviceSection> AmsBackendToolChanger::get_device_sections() const {
    // A toolhead changer carries its own extruder: only a frame-side feeder, or
    // a load this screen runs itself, has anything to expose.
    std::vector<helix::printer::DeviceSection> sections;
    if (feeder_.present) {
        sections.push_back(helix::printer::device_section(
            "feeder", 0, TR_NOOP("Release or grip the filament by hand")));
    }
    if (mounts_to_feed_) {
        sections.push_back(helix::printer::device_section(
            "purge", 1, TR_NOOP("How much a load pushes through the nozzle")));
    }
    bool can_clean = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        can_clean = !clean_nozzle_gcode_.empty();
    }
    if (can_clean) {
        sections.push_back(helix::printer::device_section(
            "maintenance", 2, TR_NOOP("Brush the mounted tool's nozzle clean")));
    }
    return sections;
}

std::vector<helix::printer::DeviceAction> AmsBackendToolChanger::get_device_actions() const {
    using DA = helix::printer::DeviceAction;
    std::vector<helix::printer::DeviceAction> actions;
    bool can_clean = false;
    bool tool_mounted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        can_clean = !clean_nozzle_gcode_.empty();
        tool_mounted = system_info_.current_tool >= 0;
    }
    if (can_clean) {
        // The command brushes whatever is on the carriage and does nothing
        // without a tool there.
        auto clean = DA::button("clean_nozzle", "Clean nozzle", "maintenance", "nozzle",
                                "Wipe the mounted nozzle on the brush")
                         .with_machine_motion();
        clean.enabled = tool_mounted;
        if (!tool_mounted) {
            clean.disable_reason = "No tool is mounted";
        }
        actions.push_back(std::move(clean));
    }
    if (mounts_to_feed_) {
        int purge = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            purge = purge_mm_;
        }
        actions.push_back(DA::slider("purge_length", "Purge Length", "purge",
                                     static_cast<float>(purge), 0.0f,
                                     static_cast<float>(helix::mount_feed::kMaxPurgeMm), "mm",
                                     "ruler", "Extruded past the nozzle after each load"));
    }
    if (!feeder_.present) {
        return actions;
    }
    actions.push_back(
        DA::button("open_feeder", "Open feeder", "feeder", "lock_open", "Release the filament"));
    actions.push_back(
        DA::button("close_feeder", "Close feeder", "feeder", "lock", "Grip the filament"));

    // Which macro each button sends. The command names forked upstream - the
    // original config exposes OPEN/CLOSE, the Python controller registers
    // MHC_OPEN/MHC_CLOSE and ships legacy aliases - and a machine mid-migration
    // can have either, both, or aliases pointing somewhere custom. Offering the
    // macros the printer actually reports beats a free-text field nobody can
    // typo-check.
    if (!feeder_.macro_options.empty()) {
        actions.push_back(DA::dropdown("feeder_open_macro", "Open feeder macro", "feeder",
                                       feeder_.macro_options, feeder_.open_choice, "",
                                       "Which macro the Open feeder button sends"));
        actions.push_back(DA::dropdown("feeder_close_macro", "Close feeder macro", "feeder",
                                       feeder_.macro_options, feeder_.close_choice, "",
                                       "Which macro the Close feeder button sends"));
    }
    return actions;
}

AmsError AmsBackendToolChanger::set_feeder_open(bool open) {
    if (!feeder_.present) {
        return AmsErrorHelper::not_supported("Feeder");
    }
    // On a hotend changer the feeder gripper is the only thing holding the
    // filament, so opening it mid-print drops the strand and kills the job.
    // Callers reach this with no gate of their own (the device-operations
    // overlay calls straight through), so this is the one place that refuses.
    // requires_toolhead_motion=true also covers the mid-tool-change window,
    // since the AMS is busy for its duration.
    if (auto refused = check_preconditions(/*requires_toolhead_motion=*/true); !refused) {
        return refused;
    }
    std::string gcode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        gcode = open ? feeder_.open_gcode : feeder_.close_gcode;
    }
    return execute_gcode(gcode);
}

AmsError AmsBackendToolChanger::clean_nozzle() {
    std::string gcode;
    bool tool_mounted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        gcode = clean_nozzle_gcode_;
        tool_mounted = system_info_.current_tool >= 0;
    }
    if (gcode.empty()) {
        return AmsErrorHelper::not_supported("Clean nozzle");
    }
    // It moves the toolhead to the brush: never under a print or a swap.
    if (auto refused = check_preconditions(/*requires_toolhead_motion=*/true); !refused) {
        return refused;
    }
    if (!tool_mounted) {
        return AmsError(AmsResult::WRONG_STATE, "Clean nozzle refused: no tool mounted",
                        lv_tr("No tool is mounted"), "");
    }
    spdlog::info("{} Cleaning the mounted nozzle: {}", backend_log_tag(), gcode);
    return execute_gcode(gcode);
}

bool AmsBackendToolChanger::feeder_open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return feeder_open_;
}

AmsError AmsBackendToolChanger::do_execute_device_action(const std::string& action_id,
                                                         const std::any& value) {
    if (mounts_to_feed_ && action_id == "purge_length") {
        const auto [mm, value_err] = printer::device_action_value<float>(value, "purge length");
        if (!value_err) {
            return value_err;
        }
        const int purge =
            std::clamp(static_cast<int>(std::lround(mm)), 0, helix::mount_feed::kMaxPurgeMm);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            purge_mm_ = purge;
        }
        helix::SettingsManager::instance().set_mount_feed_purge_mm(purge);
        spdlog::info("{} Load purge set to {}mm", backend_log_tag(), purge);
        return AmsErrorHelper::success();
    }
    if (feeder_.present && (action_id == "open_feeder" || action_id == "close_feeder")) {
        return set_feeder_open(action_id == "open_feeder");
    }
    if (action_id == "clean_nozzle") {
        return clean_nozzle();
    }
    if (feeder_.present &&
        (action_id == "feeder_open_macro" || action_id == "feeder_close_macro")) {
        const auto [chosen, err] = printer::device_action_macro_choice(value);
        if (!err) {
            return err;
        }
        const bool is_open = (action_id == "feeder_open_macro");
        // "auto" restores whatever the add-on detected for this machine, so a
        // user who migrates later picks up the new native command without
        // touching this again.
        const std::string resolved = helix::resolve_macro_choice(
            chosen, is_open ? feeder_.detected_open : feeder_.detected_close);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            (is_open ? feeder_.open_gcode : feeder_.close_gcode) = resolved;
            (is_open ? feeder_.open_choice : feeder_.close_choice) = chosen;
        }
        if (is_open) {
            helix::SettingsManager::instance().set_feeder_open_macro(chosen);
        } else {
            helix::SettingsManager::instance().set_feeder_close_macro(chosen);
        }
        spdlog::info("{} Feeder {} macro set to {} (sends {})", backend_log_tag(),
                     is_open ? "open" : "close", chosen, resolved);
        return AmsErrorHelper::success();
    }
    return AmsErrorHelper::not_supported("Device action: " + action_id);
}

} // namespace helix
