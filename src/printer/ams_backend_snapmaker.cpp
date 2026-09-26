// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_SNAPMAKER

#include "ams_backend_snapmaker.h"

#include "ui_insert_notice.h"
#include "ui_toast_manager.h"

#include "ams_error.h"
#include "ams_fault_event.h"
#include "ams_state.h"
#include "app_globals.h"
#include "batch_feed_reconcile.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "json_utils.h"
#include "klipper_extruder_naming.h"
#include "lane_legacy_migration.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "macro_patterns.h"
#include "moonraker_api.h"
#include "pause_cause.h"
#include "post_op_cooldown_manager.h"
#include "settings_manager.h"
#include "snapmaker_channel_state.h"
#include "snapmaker_resume.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <lvgl.h>
#include <string_view>
#include <utility>
#include <vector>

namespace helix {

namespace {

/// Parses a pending insert verdict may hold while waiting for the tag
/// reader's filament_detect.info entry. Status frames arrive about once a
/// second, so eight parses is roughly eight seconds: long enough for a read to
/// land, short enough that an unverified insert cannot sit silent forever.
constexpr int kSnapPendingInsertPasses = 8;

/// A digit run short enough to parse as an index without overflowing. Ten or
/// more digits exceeds the narrowest supported target's parse range, and both
/// stoi and stoul report that by throwing, so the length is checked before the
/// parse instead of the throw being caught after it. No real extruder or tool
/// index comes anywhere near that many digits.
[[nodiscard]] bool is_parsable_index(std::string_view digits) {
    return !digits.empty() && digits.size() < 10 &&
           std::all_of(digits.begin(), digits.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

// Snapmaker's recognized filament SUB_TYPE product lines. The RFID read path
// stores SUB_TYPE into SlotInfo::spool_name (see handle_status_update), but a
// user can edit spool_name to a free-form string ("My Custom Spool"). Both the
// apply_user_edit firmware round-trip (POST /printer/filament_detect/set) and the
// #991 post-runout SET_PRINT_FILAMENT_CONFIG re-assert must only treat
// spool_name as a SUB_TYPE when it matches one of these — a single source of
// truth for "is this a real product line?".
constexpr std::array<std::string_view, 8> KNOWN_SUB_TYPES = {
    "Basic", "Matte", "SnapSpeed", "Silk", "Support", "HF", "95A", "95A HF"};

[[nodiscard]] bool is_known_subtype(const std::string& s) {
    for (const auto& st : KNOWN_SUB_TYPES) {
        if (s == st) {
            return true;
        }
    }
    return false;
}

// Map a raw firmware channel_error token to a user-facing message. The firmware
// emits machine tokens (e.g. "no_filament") that are meaningless to a user.
// Unknown tokens fall back to the raw string so we never hide a novel error
// behind a generic message. Single source of truth shared by the error-set path
// here and any modal that surfaces operation_detail.
[[nodiscard]] std::string friendly_channel_error(const std::string& token, helix::ui::LaneNoun noun,
                                                 int lane_index) {
    if (token == "no_filament") {
        // The lane is a bare prefix, not part of the sentence: the frame must
        // not agree with a noun that varies per locale and backend.
        return helix::ui::lane_label(noun, lane_index) + ": " +
               lv_tr("No filament. Load filament and retry.");
    }
    return token;
}

// User-facing message for a *_fail channel_state (Change 2). The firmware
// signals the failure in channel_state itself (e.g. "load_fail") independently
// of the channel_error token, so surface a direction-aware message when a fail
// state lands. lane_index is 0-based.
[[nodiscard]] std::string friendly_channel_state_fail(const std::string& state,
                                                      helix::ui::LaneNoun noun, int lane_index) {
    const std::string lane = helix::ui::lane_label(noun, lane_index) + ": ";
    if (state.rfind("unload_", 0) == 0) {
        return lane + lv_tr("Unload failed");
    }
    if (state.rfind("preload_", 0) == 0) {
        return lane + lv_tr("Preload failed");
    }
    // load_fail, manual_sta_*_fail, and any other feed failure.
    return lane + lv_tr("Load failed");
}

using snapmaker::ChannelStateInfo;
using snapmaker::classify_channel_state;

} // namespace

// ============================================================================
// Construction
// ============================================================================

AmsBackendSnapmaker::AmsBackendSnapmaker(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Initialize system info
    system_info_.type = AmsType::SNAPMAKER;
    system_info_.type_name = "Snapmaker SnapSwap";
    system_info_.supports_bypass = false;
    system_info_.has_hardware_bypass_sensor = false;
    // The U1 has no filament cutter and forms no discrete tip — unload is just
    // heat + retract. Leaving tip_method at its CUT default mislabels the unload
    // stepper's middle phase "Cut & retract"; NONE drives the 2-step
    // "Heat nozzle -> Retract" stepper (see recreate_step_progress_for_operation).
    system_info_.tip_method = TipMethod::NONE;

    // Initialize 1 unit with 4 slots
    AmsUnit unit;
    unit.unit_index = 0;
    unit.name = "SnapSwap";
    unit.display_name = "SnapSwap";
    unit.slot_count = NUM_TOOLS;
    unit.first_slot_global_index = 0;
    unit.connected = true;
    unit.topology = PathTopology::PARALLEL;

    for (int i = 0; i < NUM_TOOLS; i++) {
        SlotInfo slot;
        slot.slot_index = i;
        slot.global_index = i;
        slot.status = SlotStatus::UNKNOWN;
        slot.mapped_tool = i;
        // Klipper uses "extruder" for T0, "extruder1" for T1, etc.
        slot.extruder_name = (i == 0) ? "extruder" : fmt::format("extruder{}", i);
        unit.slots.push_back(slot);
    }

    system_info_.units.push_back(std::move(unit));
    system_info_.total_slots = NUM_TOOLS;

    // Snapmaker U1 has a fixed 1:1 tool↔slot mapping (4 extruders, 4 slots):
    // every head permanently holds its own spool, so the PHYSICAL routing really
    // is identity. Consumers that need that read it here — the Load/Unload
    // buttons, and the persisted tool-map ledger in ams_tool_map_sync.h.
    //
    // It is NOT the print routing, and nothing may read it as such. Which head
    // prints logical tool N lives in print_task_config.extruder_map_table, which
    // get_tool_mapping() below publishes. This map was originally populated to
    // stop ui_gcode_viewer_apply_ams_tool_colors() short-circuiting on an empty
    // map (which left the model black when the first tool's filament was dark) —
    // i.e. a safety valve was fed data to keep it from tripping, and the "fix"
    // made every tool render in head-index order. That consumer now resolves
    // through the routing table instead, so this map has no display job.
    system_info_.tool_to_slot_map.reserve(NUM_TOOLS);
    for (int i = 0; i < NUM_TOOLS; i++) {
        system_info_.tool_to_slot_map.push_back(i);
    }

    spdlog::debug("[AMS Snapmaker] Backend created with {} tools", NUM_TOOLS);
}

// ============================================================================
// Lifecycle
// ============================================================================

void AmsBackendSnapmaker::set_discovery(const helix::PrinterDiscovery& discovery) {
    std::lock_guard<std::mutex> lock(mutex_);
    // One lookup answers both the script-shape question and the object
    // key: the macro's config-case name is non-empty exactly when the
    // firmware ships it.
    const std::string macro =
        discovery.macro_config_name(helix::macro_patterns::AUTO_FEEDING_BATCH);
    use_batch_macro_ = !macro.empty();
    batch_macro_object_ = macro.empty() ? std::string{} : "gcode_macro " + macro;
}

void AmsBackendSnapmaker::on_started() {
    // Load persisted per-slot overrides (brand, spool name, spoolman IDs, etc.)
    // from the Moonraker DB lane_data namespace BEFORE any status parse runs.
    // AmsSubscriptionBackend::start() registers the WebSocket subscription
    // before on_started(); a status notification could in principle fire on
    // the libhv thread while we're still inside load_blocking(). Holding
    // mutex_ only during the swap keeps the parse path's read of overrides_
    // coherent without blocking it during the 5s DB round-trip.
    if (!api_)
        return;

    auto loaded =
        helix::ams::make_loaded_override_store(api_, "snapmaker", get_type(), backend_log_tag());
    if (loaded.store) {
        helix::ams::ingest_legacy_records(*loaded.store, helix::ams::LegacyLockKeys::LaneData,
                                          backend_index());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        override_store_ = std::move(loaded.store);
        overrides_ = std::move(loaded.overrides);
        helix::ams::bind_fingerprint_persistence(rfid_tracker_, override_store_.get(), overrides_);
        // A reconnect re-baselines the feed ports: an insert pending from
        // before it is judged against nothing this session has seen.
        pending_insert_passes_.fill(0);
        feed_presence_seen_.fill(false);
    }
}

// ============================================================================
// State Queries
// ============================================================================

AmsSystemInfo AmsBackendSnapmaker::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_;
}

SlotInfo AmsBackendSnapmaker::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* slot = system_info_.get_slot_global(slot_index);
    if (slot) {
        return *slot;
    }
    SlotInfo empty;
    empty.slot_index = -1;
    return empty;
}

SlotInfo* AmsBackendSnapmaker::cached_slot_locked(int slot_index) {
    return system_info_.get_slot_global(slot_index);
}

AmsBackend::OperationStepModel
AmsBackendSnapmaker::get_operation_step_model(StepOperationType op) const {
    // Per-direction firmware step sequence. Each step's phase_id is the index the
    // classifier (classify_channel_state) emits into system_info_.operation_phase,
    // which the sidebar consumes directly as the current step index via the
    // ams_operation_phase subject. Load and unload use different-length step lists;
    // that is safe because only one backend + one operation is live at a time, so
    // Snapmaker owns the whole index space (classify_channel_state maps load/manual/
    // preload states into the LOAD indices and unload states into the UNLOAD ones).
    //
    //   LOAD  (5 steps): Home 0 -> Select 1 -> Heat 2 (live) -> Feed 3 -> Purge 4
    //     load_prepare/homing -> Home; load_picking -> Select; load_heating -> Heat;
    //     load_feeding/extruding -> Feed; load_flushing -> Purge.
    //     (preload and the manual_sta_* family reuse this load-direction model.)
    //   UNLOAD (4 steps): Home 0 -> Select 1 -> Heat 2 (live) -> Retract 3
    //     unload_prepare/homing -> Home; unload_picking -> Select;
    //     unload_heating/heat_finish -> Heat; unload_doing -> Retract.
    //
    // The Heat step (phase 2) shows a live nozzle temperature. All labels are
    // wrapped in lv_tr() so they are translated and picked up by the string tooling.
    const bool unload = (op == StepOperationType::UNLOAD);
    OperationStepModel model;
    model.steps.push_back({lv_tr("Home"), 0, false, false});
    model.steps.push_back({lv_tr("Select"), 1, false, false});
    model.steps.push_back({lv_tr("Heat nozzle"), 2, false, /*live_temp=*/true});
    if (unload) {
        model.steps.push_back({lv_tr("Retract"), 3, false, false});
    } else {
        model.steps.push_back({lv_tr("Feed filament"), 3, false, false});
        model.steps.push_back({lv_tr("Purge"), 4, false, false});
    }
    return model;
}

lv_subject_t* AmsBackendSnapmaker::get_operation_step_index_subject(StepOperationType /*op*/) {
    // The U1 firmware drives the current step directly via the operation-phase
    // subject (the per-direction step index from classify_channel_state), not via
    // narration.
    return AmsState::instance().get_ams_operation_phase_subject();
}

// ============================================================================
// Path Visualization
// ============================================================================

PathSegment AmsBackendSnapmaker::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (system_info_.current_tool >= 0 && system_info_.filament_loaded) {
        return PathSegment::NOZZLE;
    }
    return PathSegment::SPOOL;
}

PathSegment AmsBackendSnapmaker::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* slot = system_info_.get_slot_global(slot_index);
    if (!slot)
        return PathSegment::NONE;

    if (slot_index < 0 || slot_index >= NUM_TOOLS)
        return PathSegment::NONE;

    // Filament is threaded all the way into THIS tool's nozzle only when the
    // channel_state latch says load_finish. The per-tool motion sensor
    // (sensor_filament_present_) is NOT a reliable "at toolhead" signal on
    // current firmware — after an unload it lingers present (the tip parks at
    // the toolhead sensor while retracting out of the melt zone), so keying
    // NOZZLE off it left unloaded lanes rendering as fully loaded (the whole
    // point of the channel_state fix). PARALLEL multi-toolhead machine: each
    // tool feeds its own dedicated nozzle, so a loaded tool always has filament
    // at its own nozzle — render NOZZLE.
    if (loaded_at_toolhead_[slot_index]) {
        return PathSegment::NOZZLE;
    }

    // Not loaded to the nozzle. If filament is still staged in the bowden/buffer
    // — the port sensor reads present, or the motion sensor still lingers
    // present just after an unload — draw the line down to the toolhead entry
    // sensor dot but no farther (OUTPUT); the dot stays hollow because the
    // filament is not fed into the hotend. Otherwise nothing (genuine runout /
    // empty lane).
    if (port_sensor_filament_present_[slot_index] || sensor_filament_present_[slot_index]) {
        return PathSegment::OUTPUT;
    }
    return PathSegment::NONE;
}

PathSegment AmsBackendSnapmaker::infer_error_segment() const {
    return PathSegment::NONE;
}

// ============================================================================
// Filament Operations
// ============================================================================

AmsError AmsBackendSnapmaker::do_load_filament(int slot_index) {
    auto err = validate_slot_index(slot_index);
    if (err.result != AmsResult::SUCCESS)
        return err;

    // Snapmaker U1 firmware: AUTO_FEEDING is a thin macro wrapper that
    // forwards to the underlying FEED_AUTO command with module/channel
    // resolved from _FILAMENT_FEED_VARIABLE. FEED_AUTO has explicit LOAD /
    // UNLOAD / AUTO parameters — passing none of them is a silent no-op
    // (cmd_FEED_AUTO falls through every branch and returns).
    //
    // We must pass LOAD=1 to actually trigger the feed sequence. PRINTING
    // is left at the default 0 so we skip the firmware's port-input
    // filament-detected gate (which silent-returns when PRINTING=1 and the
    // port sensor reads no filament — the exact runout-recovery state).
    //
    // Trail of bad guesses, in order:
    //   1. T{n} — no-op when target tool already active (Klipper logged
    //      "Extruder extruderN already active"). That's always the case
    //      after a runout, so loads did nothing.
    //   2. AUTO_FEEDING EXTRUDER={n} — silent no-op because no LOAD
    //      parameter was passed; cmd_FEED_AUTO fell through.
    //   3. SM_PRINT_AUTO_FEED — gated on print_task_config.extruders_used,
    //      which can be all-false on a partially-extruded paused print.
    // The firmware's cmd_FEED_AUTO is the definitive reference; see
    // /home/lava/klipper/klippy/extras/filament_feed.py around line 1681.
    return execute_gcode(fmt::format("AUTO_FEEDING EXTRUDER={} LOAD=1", slot_index));
}

AmsError AmsBackendSnapmaker::do_unload_filament(int slot_index) {
    // Unload must mirror load: route through AUTO_FEEDING (the firmware macro
    // that forwards to FEED_AUTO with module/channel resolved from
    // _FILAMENT_FEED_VARIABLE), passing UNLOAD=1. FEED_AUTO with no STAGE runs
    // the full unload state machine (prepare → home → pick → heat → unload →
    // finish), which drives the firmware's per-channel feed state all the way
    // to "unload_finish".
    //
    // The bare INNER_FILAMENT_UNLOAD macro is the *leaf* the state machine
    // invokes internally — calling it directly skips the state transitions.
    // On a plain U1 that's fine, but it breaks aftermarket feeders that hook
    // the unload-finish state: the DnG-Crafts U1-Ace mod retracts the Anycubic
    // ACE Pro spool only when a channel reaches "unload_finish", so the bare
    // macro left filament dangling at the toolhead (prestonbrown/helixscreen#974).
    //
    // Resolve which extruder to unload: callers usually pass nothing (-1,
    // "the currently loaded one"), so fall back to current_slot. If we still
    // don't know which slot is loaded, fall back to the firmware's bare unload
    // rather than guess an EXTRUDER index.
    int extruder = slot_index;
    if (extruder < 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        extruder = system_info_.current_slot;
    }
    if (extruder < 0) {
        return execute_gcode("INNER_FILAMENT_UNLOAD");
    }

    auto err = validate_slot_index(extruder);
    if (err.result != AmsResult::SUCCESS)
        return err;

    return execute_gcode(fmt::format("AUTO_FEEDING EXTRUDER={} UNLOAD=1", extruder));
}

AmsError AmsBackendSnapmaker::do_filament_batch(const std::vector<int>& slots, bool load) {
    if (slots.empty()) {
        return AmsErrorHelper::invalid_parameter("batch filament op with no slots");
    }
    for (int slot : slots) {
        if (auto err = validate_slot_index(slot); err.result != AmsResult::SUCCESS) {
            return err;
        }
    }
    if (!api_) {
        return AmsErrorHelper::not_connected("IMoonrakerAPI not available");
    }
    bool use_batch_macro;
    uint64_t dispatch_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The in-flight claim spans only this dispatch call, and `action`
        // returns to IDLE at each head's terminal — so the preheat gap
        // between heads would otherwise admit a second batch whose
        // ACTION=START the firmware's `doing` interlock refuses, and that
        // refusal fires the RPC-error recovery's ACTION=END into the batch
        // still running.
        if (batch_.active) {
            return AmsErrorHelper::busy(
                ams_action_to_string(batch_.load ? AmsAction::LOADING : AmsAction::UNLOADING));
        }
        use_batch_macro = use_batch_macro_;
        // Resolve the progress words here on the caller's (main) thread: the
        // cursor-advance parse reads them from the WebSocket thread, which
        // must not call lv_tr into LVGL's pack list.
        batch_ = BatchPlan{slots,           load,
                           /*cursor=*/0,
                           /*active=*/true, load ? lv_tr("Load") : lv_tr("Unload"),
                           lv_tr("of"),     next_batch_dispatch_id_};
        dispatch_id = next_batch_dispatch_id_++;
    }
    const std::string chain = batch_feed_gcode(slots, load, use_batch_macro);
    const char* tag = backend_log_tag();
    spdlog::info("{} Executing G-code: {}", tag, chain);
    // Sent through api_ rather than the shared execute_gcode() so the timeout
    // can scale per op — the shared overloads pin AMS_OPERATION_TIMEOUT_MS
    // (300s), which a 4-head cold batch can outlast. The operation's
    // completion is owned by the firmware phase the sidebar tracks, not by
    // this RPC's return; the error path reaches `this` only through the
    // lifetime token, which marshals to main and skips a dead owner.
    auto tok = lifetime_.token();
    api_->execute_gcode(
        chain, [tag]() { spdlog::debug("{} batch G-code executed successfully", tag); },
        [this, tok, tag, chain, dispatch_id](const MoonrakerError& err) mutable {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("{} G-code response timed out (may still be running): {}", tag, chain);
            } else {
                spdlog::error("{} G-code failed: {} - {}", tag, chain, err.message);
            }
            tok.defer("AmsBackendSnapmaker::do_filament_batch.recover",
                      [this, tag, msg = err.message, dispatch_id] {
                          // The failure carries authority only while the plan
                          // it failed is still the live one: a TIMEOUT can
                          // land long after every head verified, and END
                          // then restores the targets snapshotted at that
                          // batch's START over a preheat the user started
                          // since.
                          bool clear_interlock;
                          int failed_head;
                          {
                              std::lock_guard<std::mutex> lock(mutex_);
                              clear_interlock = batch_.active && batch_.dispatch_id == dispatch_id;
                              failed_head = clear_interlock ? batch_.heads[batch_.cursor] : -1;
                              if (clear_interlock) {
                                  // The recovery's own END finishes the
                                  // batch: leaving the plan active would let
                                  // later unrelated channel traffic advance a
                                  // zombie cursor.
                                  batch_.active = false;
                              }
                          }
                          if (!clear_interlock) {
                              spdlog::info("{} batch RPC failure for dispatch {} is stale — "
                                           "interlock left alone",
                                           tag, dispatch_id);
                              return;
                          }
                          spdlog::warn("{} batch RPC failed at head {} ({}): clearing the "
                                       "firmware batch interlock",
                                       tag, failed_head, msg);
                          end_firmware_batch();
                      });
        },
        static_cast<uint32_t>(slots.size()) * BATCH_FEED_OP_TIMEOUT_MS,
        /*silent=*/true, /*on_queued=*/nullptr,
        // The callbacks above log and schedule the interlock clear; neither
        // claims the error report, so Klipper's `!!` broadcast still surfaces
        // — the explanation a failed feed actually needs.
        /*caller_surfaces_errors=*/false);
    return AmsErrorHelper::success();
}

std::string AmsBackendSnapmaker::batch_feed_gcode(const std::vector<int>& slots, bool load,
                                                  bool use_batch_macro) {
    const char* dir = load ? "LOAD=1" : "UNLOAD=1";
    if (!use_batch_macro) {
        std::string chain;
        for (int slot : slots) {
            if (!chain.empty()) {
                chain += '\n';
            }
            chain += fmt::format("AUTO_FEEDING EXTRUDER={} {}", slot, dir);
        }
        return chain;
    }

    // START snapshots every hotend target and raises the `doing` interlock;
    // END restores those targets mid-print and zeroes them when idle. The
    // firmware refuses a print start while `doing` is set, so END must run.
    // NEXT_EXTRUDER names the next selected head so the firmware preheats it
    // while the current one runs; it is omitted on the last.
    std::string chain = "AUTO_FEEDING_BATCH ACTION=START";
    for (size_t i = 0; i < slots.size(); ++i) {
        chain += fmt::format("\nAUTO_FEEDING_BATCH ACTION=DOING EXTRUDER={} {}", slots[i], dir);
        if (i + 1 < slots.size()) {
            chain += fmt::format(" NEXT_EXTRUDER={}", slots[i + 1]);
        }
    }
    chain += '\n';
    chain += batch_feeding::END_GCODE;
    return chain;
}

AmsBackendSnapmaker::BatchPlan AmsBackendSnapmaker::batch_plan() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return batch_;
}

void AmsBackendSnapmaker::end_firmware_batch() {
    bool use_batch_macro;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        use_batch_macro = use_batch_macro_;
    }
    if (!use_batch_macro) {
        return; // no interlock exists on this firmware
    }
    execute_gcode(batch_feeding::END_GCODE);
}

bool AmsBackendSnapmaker::can_unload_from_toolhead(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= NUM_TOOLS) {
        return false;
    }
    const auto* slot = system_info_.get_slot_global(slot_index);
    if (!slot || !slot->is_present()) {
        return false;
    }
    // Filament must be AT this toolhead, not merely parked in the buffer. The
    // channel_state latch reads true only between load_finish and the next
    // unload_finish/wait_insert/preload_finish. The per-tool motion sensor
    // (e{N}_filament) is NOT a reliable load signal on current firmware — it
    // stays true after an unload — so it must not gate Unload. Without the
    // latch the menu kept offering Unload for an already-unloaded tool. See the
    // header note + the u1_channel_state_reference.md live capture.
    return loaded_at_toolhead_[slot_index];
}

bool AmsBackendSnapmaker::slot_has_filament_at_toolhead(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= NUM_TOOLS) {
        return false;
    }
    // channel_state latch, NOT the motion sensor — see can_unload_from_toolhead.
    return loaded_at_toolhead_[slot_index];
}

bool AmsBackendSnapmaker::slot_is_actively_loaded(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= NUM_TOOLS) {
        return false;
    }
    const auto* slot = system_info_.get_slot_global(slot_index);
    return slot && slot->status == SlotStatus::LOADED;
}

AmsBackendSnapmaker::ChannelSnapshot AmsBackendSnapmaker::channel_snapshot(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= NUM_TOOLS) {
        return {};
    }
    return channel_snapshots_[slot_index];
}

AmsBackend::FilamentOpEligibility AmsBackendSnapmaker::slot_op_eligibility(int slot_index,
                                                                           bool load) const {
    using E = FilamentOpEligibility;
    if (slot_index < 0 || slot_index >= NUM_TOOLS) {
        return E::Busy;
    }
    const ChannelSnapshot snap = channel_snapshot(slot_index);

    // The firmware reports channel_error="no_filament" for ANY empty lane, and
    // ""/"none" when a channel has nothing to say — none of the three is a
    // hard fault, or every empty feeder would read as an error.
    const bool hard_fault = snap.error != "ok" && !snap.error.empty() && snap.error != "none" &&
                            snap.error != "no_filament";
    if (hard_fault) {
        return E::Error;
    }
    // An empty lane answers Empty ahead of the settled-state test: an idle
    // empty lane reports an unsettled state ("none"/"inited"), and presence
    // is knowable even when the state vocabulary is not.
    if (!snap.filament_detected) {
        return E::Empty;
    }
    // Only these five are settled states. Anything else is mid-operation or
    // unrecognised, and a batch must not act on a head it cannot describe.
    // manual_sta_finish is a persistent terminal: a head that finished a
    // manual feed sits in it until the next operation, so reading it as
    // Busy would refuse every batch naming that head.
    const bool settled = snap.state == "wait_insert" || snap.state == "preload_finish" ||
                         snap.state == "load_finish" || snap.state == "unload_finish" ||
                         snap.state == "manual_sta_finish";
    if (!settled) {
        return E::Busy;
    }
    const bool loaded = snap.state == "load_finish";
    if (load && loaded) {
        return E::AlreadyLoaded;
    }
    if (!load && !loaded) {
        return E::NotLoaded;
    }
    // Eligible on state; now the feeder has to be able to act.
    if (!snap.module_exist || snap.disable_auto) {
        return E::FeederUnavailable;
    }
    if (load && !snap.sensor_enabled) {
        return E::SensorDisabled;
    }
    return E::Eligible;
}

AmsError AmsBackendSnapmaker::do_select_slot(int slot_index) {
    return do_change_tool(slot_index);
}

AmsError AmsBackendSnapmaker::do_change_tool(int tool_number) {
    auto err = validate_slot_index(tool_number);
    if (err.result != AmsResult::SUCCESS)
        return err;

    return execute_gcode(fmt::format("T{}", tool_number));
}

// ============================================================================
// Recovery (not supported)
// ============================================================================

AmsError AmsBackendSnapmaker::recover() {
    return AmsErrorHelper::not_supported("Recover not supported on Snapmaker");
}

AmsError AmsBackendSnapmaker::reset() {
    return AmsErrorHelper::not_supported("Reset not supported on Snapmaker");
}

AmsError AmsBackendSnapmaker::cancel() {
    return AmsErrorHelper::not_supported("Cancel not supported on Snapmaker");
}

// ============================================================================
// Resume Preparation
// ============================================================================

// NOTE: This method has no caller in tree right now. The auto-recover-on-pause
// path that consumed it was pulled after field testing showed the
// motion=false + port=true signal can't distinguish "stale encoder, filament
// at gear" from "filament preloaded 4-inches short of gear" (firmware assist
// motor stops at preload_finish). Kept as detection infrastructure for the
// deferred follow-up in task #19 — when we have a verifiable signal that
// filament is *at* the extruder gear (likely filament_feed.channel_state ==
// 'load_finish'), this gate logic gets revived and tightened.
bool AmsBackendSnapmaker::is_stuck_motion_sensor_runout(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int slot = slot_index;
    if (slot < 0) {
        // Snapmaker has 1:1 tool↔slot mapping — current_tool and current_slot
        // hold the same value. Either is correct; current_slot matches the
        // convention used by other AmsBackend methods that take slot_index.
        slot = system_info_.current_slot;
    }
    if (slot < 0 || slot >= NUM_TOOLS) {
        return false;
    }
    // Motion (encoder) sensor latched false, port (buffer) sensor still
    // sees filament. The encoder is stale — the slot has physical filament
    // ready to feed, the motion sensor just hasn't been re-armed by extrusion.
    return !sensor_filament_present_[slot] && port_sensor_filament_present_[slot];
}

AmsBackendSnapmaker::~AmsBackendSnapmaker() = default;

void AmsBackendSnapmaker::prepare_for_resume(int slot_index, ResumeReadyCallback on_ready) {
    // Resolve target slot. Caller passes -1 when they don't know which tool
    // is active — fall back to system_info_.current_tool. If still unset, no
    // active tool means there's nothing to prep; just unblock the caller.
    int slot = slot_index;
    bool sensor_present = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot < 0) {
            // Snapmaker has 1:1 tool↔slot mapping — current_tool and
            // current_slot hold the same value. current_slot matches the
            // slot_index convention used by other AmsBackend methods.
            slot = system_info_.current_slot;
        }
        if (slot >= 0 && slot < NUM_TOOLS) {
            sensor_present = sensor_filament_present_[slot];
        }
    }

    if (slot < 0 || slot >= NUM_TOOLS) {
        spdlog::debug("{} prepare_for_resume: no active tool, skipping prep", backend_log_tag());
        if (on_ready) {
            on_ready(AmsErrorHelper::success());
        }
        return;
    }

    // Classify why the print paused. Terminal causes (confirmed under #991)
    // get the restart modal up front; everything else attempts RESUME
    // (default-recoverable). A RESUME that truly no-ops returns an ERROR through
    // the RESUME gcode callback, which the dispatch layer handles. Replaces the
    // old blunt virtual_sdcard gate.
    helix::PauseSignals sig;
    sig.exception_id = get_printer_state().get_print_exception_id();
    sig.exception_code = get_printer_state().get_print_exception_code();
    // On these firmware pauses print_stats.message is empty — the reason text
    // lives in exception.message. Fall back to print_stats.message when the
    // exception carries no text (e.g. non-Snapmaker pause paths).
    sig.message = get_printer_state().get_print_exception_message();
    if (sig.message.empty()) {
        sig.message = lv_subject_get_string(get_printer_state().get_print_message_subject());
    }
    sig.sdcard_active = get_printer_state().is_sdcard_active();
    sig.runout_tripped = !sensor_present;
    if (helix::classify_pause(sig, helix::snapmaker_terminal_matchers()) ==
        helix::PauseCause::Terminal) {
        spdlog::warn("{} prepare_for_resume: classified Terminal — surfacing restart UX",
                     backend_log_tag());
        if (on_ready) {
            // snapmaker_terminal_matchers() currently recognizes one terminal
            // cause (dirty bed), so a single fixed reason covers every match.
            on_ready(AmsErrorHelper::resume_requires_restart(
                "classify_pause: Terminal",
                lv_tr("The bed was reported dirty, so this print cannot resume.")));
        }
        return;
    }

    if (sensor_present) {
        // Klipper's motion sensor reads filament — RESUME can clear its own
        // exception once extrusion resumes. No backend prep needed.
        spdlog::info("{} prepare_for_resume: tool {} sensor reports filament present, "
                     "skipping recovery",
                     backend_log_tag(), slot);
        if (on_ready) {
            on_ready(AmsErrorHelper::success());
        }
        return;
    }

    // Sensor reads runout. The firmware's own INNER_RESUME auto-feed
    // (SM_PRINT_AUTO_FEED) is gated on print_task_config.extruders_used[slot],
    // which the firmware freezes False for the duration of a print — so a plain
    // RESUME no-ops the feed and immediately re-pauses at CHECK_FILAMENT_RUNOUT.
    // AUTO_FEEDING calls FEED_AUTO directly (no extruders_used gate): it homes,
    // switches to the tool, feeds filament from the AMS port across the gap to
    // the toolhead sensor, heats to the slot's filament temp, then extrudes and
    // flushes — leaving the channel at load_finish with the runout sensor reading
    // present, so the subsequent RESUME's CHECK_FILAMENT_RUNOUT passes. The gcode
    // blocks until load_finish (success) or raises (error), and is idempotent
    // (FEED_AUTO returns early if already loaded). Verified live on a physical U1
    // (#991); replaces the old chain whose SET_FILAMENT_SENSOR ENABLE=0 silently
    // neutered FEED_AUTO and whose SET_PRINT_FILAMENT_CONFIG / manual extrude were
    // both unnecessary (INNER_RESUME restores config; AUTO_FEEDING does the heat).
    if (!api_) {
        spdlog::warn("{} prepare_for_resume: IMoonrakerAPI unavailable", backend_log_tag());
        if (on_ready) {
            on_ready(AmsErrorHelper::not_connected("IMoonrakerAPI unavailable"));
        }
        return;
    }

    std::string chain = fmt::format("AUTO_FEEDING EXTRUDER={0} LOAD=1 PRINTING=1", slot);
    spdlog::info("{} prepare_for_resume: tool {} runout latched — driving AMS load "
                 "(AUTO_FEEDING) before RESUME",
                 backend_log_tag(), slot);

    // AUTO_FEEDING heats + feeds + flushes (~86s) before the resume lands. The
    // pending-action UI only shows an optimistic spinner with no text, so tell
    // the user what the wait is — otherwise the long pause reads as a hang.
    // prepare_for_resume runs on the main thread (resume-button path), so this
    // toast is safe to raise directly here.
    ToastManager::instance().show(ToastSeverity::INFO,
                                  lv_tr("Refeeding filament — this may take a minute"),
                                  /*duration_ms=*/8000);

    auto tok = lifetime_.token();
    const char* tag = backend_log_tag();
    IMoonrakerAPI* api_ptr = api_;
    api_ptr->execute_gcode(
        chain,
        [this, tok, on_ready, tag, slot]() mutable {
            // IMoonrakerAPI callbacks fire on the libhv WebSocket thread; defer to
            // main so on_ready (and the backstop arm) run on the UI thread.
            tok.defer("AmsBackendSnapmaker::prepare_for_resume.ok",
                      [this, cb = std::move(on_ready), tag, slot]() {
                          spdlog::info("{} prepare_for_resume: tool {} AMS load complete "
                                       "(load_finish)",
                                       tag, slot);
                          // Hand control back so the caller dispatches RESUME.
                          if (cb) {
                              cb(AmsErrorHelper::success());
                          }
                      });
        },
        [this, tok, on_ready, tag, slot](const MoonrakerError& err) mutable {
            std::string msg = err.message;
            tok.defer("AmsBackendSnapmaker::prepare_for_resume.err",
                      [this, cb = std::move(on_ready), tag, slot, msg]() {
                          spdlog::error("{} prepare_for_resume: tool {} AMS load failed: {}", tag,
                                        slot, msg);
                          // Load failed → RESUME is never dispatched; report failure.
                          // Name the slot, not the firmware's own error text, which
                          // may spell it with a 0-based extruder name we don't own.
                          if (cb) {
                              // Bare prefix before the colon — the frame must not agree
                              // with a noun that varies per locale and backend.
                              cb(AmsError(AmsResult::COMMAND_FAILED,
                                          "prepare_for_resume AMS load failed: " + msg,
                                          helix::ui::lane_label(lane_noun(), slot) + ": " +
                                              lv_tr("Filament reload failed")));
                          }
                      });
        },
        // AUTO_FEEDING heats from cold + feeds + flushes; measured ~86s live, so
        // give generous headroom.
        /*timeout_ms=*/150000,
        /*silent=*/true);
}

// ============================================================================
// Configuration
// ============================================================================

namespace {

/// Put @p info's filament fields on @p slot, covering every SlotInfo field the
/// caller may set, so the UI does not snap back on the next get_slot_info read.
void write_filament_fields(SlotInfo& slot, const SlotInfo& info) {
    slot.color_name = info.color_name;
    slot.color_rgb = info.color_rgb;
    slot.material = info.material;
    slot.brand = info.brand;
    // Carry the catalog product identity through a sync too: one that dropped
    // it would make the editor snap back to a different variant on the next
    // get_slot_info().
    slot.catalog_id = info.catalog_id;
    slot.product_name = info.product_name;
    slot.nozzle_temp_min = info.nozzle_temp_min;
    slot.nozzle_temp_max = info.nozzle_temp_max;
    slot.bed_temp = info.bed_temp;
    slot.remaining_weight_g = info.remaining_weight_g;
    slot.total_weight_g = info.total_weight_g;
    slot.spoolman_id = info.spoolman_id;
    slot.spoolman_filament_id = info.spoolman_filament_id;
    slot.spoolman_vendor_id = info.spoolman_vendor_id;
    slot.spool_name = info.spool_name;
}

} // namespace

AmsError AmsBackendSnapmaker::apply_user_edit(int slot_index, const SlotInfo& info,
                                              const helix::ams::Observation& declared) {
    auto err = validate_slot_index(slot_index);
    if (err.result != AmsResult::SUCCESS)
        return err;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot = system_info_.units[0].get_slot(slot_index);
        if (!slot)
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, NUM_TOOLS - 1);

        write_filament_fields(*slot, info);

        // handle_status_update writes RFID and print_task_config fields
        // unconditionally, so an edit kept only in memory is wiped by the next
        // Klipper status update. Stage the override into overrides_ so the edit
        // survives a restart; the lane's own declaration, filed when the edit is
        // committed, is what apply_resolved_lane lays back over firmware data on
        // every subsequent parse.
        //
        // NOTE on self-wipe: the AD5X IFS implementation pre-updates
        // last_firmware_color_ here to prevent the color-based hardware-event
        // check from misreading a user color edit as a physical spool swap.
        // Snapmaker's hardware-event check is RFID-UID-based, and the user
        // cannot set a CARD_UID through the edit UI — SlotInfo has no UID
        // field. So rfid_tracker_ keeps whatever the firmware last
        // reported, and the next parse compares firmware UID against that
        // baseline exactly as intended. No expected-echo value needed here.
        // (CFS shares the tracker and DOES register one — it writes
        // color_value back to the box, which is half of its fingerprint.)
        helix::ams::stage_user_override(overrides_, slot_index, info, declared);
    }

    if (override_store_) {
        helix::ams::persist_staged_override(override_store_.get(), mutex_, overrides_, slot_index,
                                            backend_log_tag(), "Override");
    }

    // Push the user's edit back to firmware via the paxx12 Extended Firmware
    // POST /printer/filament_detect/set endpoint (see filament_detect.md in
    // SnapmakerU1-Extended-Firmware/docs/design/). Firmware mirrors the body
    // into print_task_config.filament_vendor / filament_type / filament_color_rgba,
    // which the parse path here already reads — so on the next status update
    // OverwriteAlways auto-mirror sees firmware-truth that matches user-truth
    // and lane_data converges. On stock firmware (no extension) the endpoint
    // 404s; the override is still persisted to lane_data, so HelixScreen's UI
    // works correctly. Only OrcaSlicer's MoonrakerPrinterAgent and the
    // firmware-side LCD don't reflect user edits on stock firmware.
    if (api_) {
        nlohmann::json info_obj = nlohmann::json::object();
        if (!info.brand.empty())
            info_obj["VENDOR"] = info.brand;
        if (!info.material.empty())
            info_obj["MAIN_TYPE"] = info.material;
        // SUB_TYPE is restricted to Snapmaker's known product lines per the
        // firmware spec. spool_name carries the SUB_TYPE on the read path
        // (see handle_status_update), but UI-edited spool_name may be a free-
        // form string ("My Custom Spool"). Only round-trip when it matches a
        // known sub_type — otherwise omit and let firmware preserve whatever
        // it had. The free-form string still lives in lane_data. Shares the
        // is_known_subtype() helper with the #991 resume re-assert path.
        if (is_known_subtype(info.spool_name)) {
            info_obj["SUB_TYPE"] = info.spool_name;
        }
        info_obj["RGB_1"] = info.color_rgb;
        info_obj["ALPHA"] = 255;
        if (info.nozzle_temp_min > 0)
            info_obj["HOTEND_MIN_TEMP"] = info.nozzle_temp_min;
        if (info.nozzle_temp_max > 0)
            info_obj["HOTEND_MAX_TEMP"] = info.nozzle_temp_max;
        if (info.bed_temp > 0)
            info_obj["BED_TEMP"] = info.bed_temp;
        // CARD_UID and SKU intentionally omitted — SlotInfo doesn't carry
        // them and we want firmware to preserve whatever the RFID tag wrote.

        nlohmann::json payload = nlohmann::json::object();
        payload["channel"] = slot_index;
        payload["info"] = info_obj;

        // Remember what the USER declared, so the RFID parse can tell firmware
        // repeating their choice back from a tag stating it.
        //
        // Recorded before dispatch, because the guard has to be armed before
        // any echo can arrive. A write firmware never accepted disarms it from
        // the response callback below.
        // Zero when the slot is out of range and nothing was staged; the
        // matched abandon() below then finds no entry and drops nothing.
        std::uint64_t staged_sequence = 0;
        if (slot_index >= 0 && slot_index < NUM_TOOLS) {
            std::lock_guard<std::mutex> lock(mutex_);
            staged_sequence = own_write_echoes_.stage(slot_index, declared);
            if (auto* staged = own_write_echoes_.staged(slot_index)) {
                // The POST has to have carried the key. A field the user
                // cleared is omitted from the body, so firmware keeps the
                // tag's value and what returns is the tag's, not theirs. RGB_1
                // is sent unconditionally and needs no such drop.
                //
                // The POST spells colour RGB_1 and the parse reads ARGB_COLOR.
                // Whether firmware translates between the two is not answered
                // anywhere in the tree or in the firmware doc, so this assumes
                // it does. If it does not, the declared colour simply never
                // matches an incoming reading and the guard is inert.
                if (!info_obj.contains("VENDOR"))
                    staged->brand.reset();
                if (!info_obj.contains("MAIN_TYPE"))
                    staged->material.reset();
                // SUB_TYPE goes out as the user's spool_name and comes back as
                // the product line, which is the field the RFID parse files it
                // under. Relocating here is what lets the shared guard stay a
                // plain field-by-field filter.
                if (info_obj.contains("SUB_TYPE"))
                    staged->product_name = staged->spool_name;
                staged->spool_name.reset();
            }
            // An unread channel arms against "no tag yet", so the first UID to
            // arrive is a reading and ends the suppression.
            own_write_echoes_.arm(slot_index,
                                  rfid_tracker_.baseline(slot_index).value_or(std::string{}));
        }

        // Routes through MoonrakerRestAPI, which dispatches on its own HTTP
        // worker thread, NOT a raw std::thread (lesson L083: pthread EAGAIN on
        // AD5M / CC1 / MIPS32). The backend can be gone by the time this fires,
        // so `this` is only ever reached through the lifetime token, which
        // marshals to the main thread and skips a dead owner.
        const std::string tag = backend_log_tag();
        auto tok = lifetime_.token();
        api_->rest().call_rest_post(
            "/printer/filament_detect/set", payload,
            [this, tok, tag, slot_index, staged_sequence](const RestResponse& resp) mutable {
                bool accepted = resp.success;
                if (!resp.success) {
                    // 404 on stock firmware (no Extended Firmware extension)
                    // is expected — log at debug, not warn, so we don't spam
                    // every user without the firmware update.
                    if (resp.status_code == 404) {
                        spdlog::debug("{} filament_detect/set unavailable (slot {}): "
                                      "stock firmware without Extended Firmware extension",
                                      tag, slot_index);
                    } else {
                        spdlog::warn("{} filament_detect/set failed for slot {}: HTTP {} {}", tag,
                                     slot_index, resp.status_code, resp.error);
                    }
                } else if (resp.data.is_object()) {
                    // Success-shaped HTTP response can still carry
                    // "state":"error" (per filament_detect.md). Drain that as a
                    // warn; the override is still saved to lane_data so user
                    // data isn't lost.
                    auto state_it = resp.data.find("state");
                    if (state_it != resp.data.end() && state_it->is_string() &&
                        state_it->get<std::string>() == "error") {
                        std::string msg;
                        auto msg_it = resp.data.find("message");
                        if (msg_it != resp.data.end() && msg_it->is_string()) {
                            msg = msg_it->get<std::string>();
                        }
                        spdlog::warn("{} filament_detect/set returned error for slot {}: {}", tag,
                                     slot_index, msg);
                        accepted = false;
                    }
                }
                if (accepted) {
                    return;
                }
                // Firmware holds none of these values, so nothing is going to
                // echo them back. Leaving the guard armed withholds the next
                // genuine tag reading until the UID changes, which is the harm
                // it exists to prevent, pointed the other way. Stock firmware
                // has no such endpoint at all, so this is the common path.
                // Matched to the staging this response answers: the user can
                // have saved a second edit meanwhile, whose guard this failure
                // has no claim on.
                tok.defer("AmsBackendSnapmaker::apply_user_edit.abandon_echo",
                          [this, slot_index, staged_sequence]() {
                              std::lock_guard<std::mutex> lock(mutex_);
                              own_write_echoes_.abandon(slot_index, staged_sequence);
                          });
            });
    }

    // Pass slot_index as event data so AmsState can do a targeted slot sync.
    // Without it, AmsState::on_event silently skips the refresh and the AMS
    // panel never re-reads the edited slot — the UI shows stale data until
    // the next firmware status notification triggers a full refresh.
    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

AmsError AmsBackendSnapmaker::sync_external_identity(int slot_index, const SlotInfo& info) {
    auto err = validate_slot_index(slot_index);
    if (err.result != AmsResult::SUCCESS)
        return err;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot = system_info_.units[0].get_slot(slot_index);
        if (!slot)
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, NUM_TOOLS - 1);

        // overrides_ is left alone and nothing reaches firmware: the next
        // Klipper status update overwrites a synced value.
        write_filament_fields(*slot, info);
    }

    // Pass slot_index as event data so AmsState can do a targeted slot sync.
    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

void AmsBackendSnapmaker::persist_external_identity_impl(int slot_index,
                                                         const helix::ams::Observation& spoolman) {
    const std::string tag = backend_log_tag();
    std::lock_guard<std::mutex> lock(mutex_);
    helix::ams::persist_override_external_identity(override_store_.get(), overrides_, slot_index,
                                                   spoolman, tag);
}

void AmsBackendSnapmaker::persist_slot_weight(int slot_index, float remaining_weight_g,
                                              float total_weight_g) {
    const std::string tag = backend_log_tag();
    std::lock_guard<std::mutex> lock(mutex_);
    helix::ams::persist_override_weight(override_store_.get(), overrides_, slot_index,
                                        remaining_weight_g, total_weight_g, tag);
}

AmsError AmsBackendSnapmaker::set_tool_mapping_impl(int /*tool_number*/, int /*slot_index*/) {
    return AmsErrorHelper::not_supported("Tool mapping not supported on Snapmaker");
}

// ============================================================================
// Bypass (not applicable)
// ============================================================================

AmsError AmsBackendSnapmaker::enable_bypass() {
    return AmsErrorHelper::not_supported("Bypass not supported on Snapmaker");
}

AmsError AmsBackendSnapmaker::disable_bypass() {
    return AmsErrorHelper::not_supported("Bypass not supported on Snapmaker");
}

// ============================================================================
// Static Parsers
// ============================================================================

ExtruderToolState AmsBackendSnapmaker::parse_extruder_state(const nlohmann::json& json) {
    ExtruderToolState state;

    if (json.contains("state") && json["state"].is_string()) {
        state.state = json["state"].get<std::string>();
    }
    if (json.contains("park_pin") && json["park_pin"].is_boolean()) {
        state.park_pin = json["park_pin"].get<bool>();
    }
    if (json.contains("active_pin") && json["active_pin"].is_boolean()) {
        state.active_pin = json["active_pin"].get<bool>();
    }
    if (json.contains("activating_move") && json["activating_move"].is_boolean()) {
        state.activating_move = json["activating_move"].get<bool>();
    }
    if (json.contains("extruder_offset") && json["extruder_offset"].is_array()) {
        const auto& arr = json["extruder_offset"];
        for (size_t i = 0; i < std::min(arr.size(), size_t{3}); i++) {
            if (arr[i].is_number()) {
                state.extruder_offset[i] = arr[i].get<float>();
            }
        }
    }
    if (json.contains("switch_count") && json["switch_count"].is_number()) {
        state.switch_count = json["switch_count"].get<int>();
    }
    if (json.contains("retry_count") && json["retry_count"].is_number()) {
        state.retry_count = json["retry_count"].get<int>();
    }
    if (json.contains("error_count") && json["error_count"].is_number()) {
        state.error_count = json["error_count"].get<int>();
    }

    return state;
}

SnapmakerRfidInfo AmsBackendSnapmaker::parse_rfid_info(const nlohmann::json& json) {
    SnapmakerRfidInfo info;

    if (json.contains("MAIN_TYPE") && json["MAIN_TYPE"].is_string()) {
        info.main_type = json["MAIN_TYPE"].get<std::string>();
    }
    if (json.contains("SUB_TYPE") && json["SUB_TYPE"].is_string()) {
        info.sub_type = json["SUB_TYPE"].get<std::string>();
    }
    if (json.contains("MANUFACTURER") && json["MANUFACTURER"].is_string()) {
        info.manufacturer = json["MANUFACTURER"].get<std::string>();
    }
    if (json.contains("VENDOR") && json["VENDOR"].is_string()) {
        info.vendor = json["VENDOR"].get<std::string>();
    }
    if (json.contains("ARGB_COLOR") && json["ARGB_COLOR"].is_number()) {
        // ARGB -> RGB: mask off the alpha byte
        uint32_t argb = json["ARGB_COLOR"].get<uint32_t>();
        info.color_rgb = argb & 0x00FFFFFF;
    }
    if (json.contains("HOTEND_MIN_TEMP") && json["HOTEND_MIN_TEMP"].is_number()) {
        info.hotend_min_temp = json["HOTEND_MIN_TEMP"].get<int>();
    }
    if (json.contains("HOTEND_MAX_TEMP") && json["HOTEND_MAX_TEMP"].is_number()) {
        info.hotend_max_temp = json["HOTEND_MAX_TEMP"].get<int>();
    }
    if (json.contains("BED_TEMP") && json["BED_TEMP"].is_number()) {
        info.bed_temp = json["BED_TEMP"].get<int>();
    }
    if (json.contains("WEIGHT") && json["WEIGHT"].is_number()) {
        info.weight_g = json["WEIGHT"].get<int>();
    }
    // CARD_UID is a 4-byte array like [144, 32, 196, 2]. Canonicalize to a
    // comma-joined string so the override system's baseline comparison is a
    // simple string == string check. Empty / missing array stays as empty
    // string (treated as "no tag / unread" by check_hardware_event_clear).
    if (json.contains("CARD_UID") && json["CARD_UID"].is_array()) {
        const auto& arr = json["CARD_UID"];
        std::string uid;
        for (size_t i = 0; i < arr.size(); ++i) {
            if (!arr[i].is_number()) {
                // If any byte isn't a number, bail out — partial UIDs aren't
                // safe to compare. Leave info.uid empty so the check is a
                // no-op for this parse.
                uid.clear();
                break;
            }
            if (!uid.empty())
                uid.push_back(',');
            uid += std::to_string(arr[i].get<int>());
        }
        info.uid = std::move(uid);
    }

    return info;
}

// ============================================================================
namespace {

/// The tracker fingerprint for one RFID reading, built from exactly the fields
/// the insert rule judges (prestonbrown/helixscreen#1710): the UID when the
/// read produced one, else the tag's material and colour when the tag decoded
/// but its UID did not, else "-" for a finished read that found no tag. A bare
/// UID is the spelling records already persist as helix_fingerprint, so it
/// cannot grow a prefix without every restart reading as a swap.
std::string fingerprint_from_evidence(const helix::ams::SpoolEvidence& evidence) {
    if (!evidence.tag_uid.empty()) {
        return evidence.tag_uid;
    }
    if (!evidence.material.empty() || evidence.color_rgb.has_value()) {
        return fmt::format("M|{}|{:06X}", evidence.material, evidence.color_rgb.value_or(0u));
    }
    if (evidence.tag_read_complete) {
        return "-";
    }
    return "";
}

/// The reading a fingerprint names, for the comparison side of the insert
/// rule. Inverse of fingerprint_from_evidence(); "" names no reading. A stored
/// fingerprint is foreign input, so a colour that does not parse reads as none
/// rather than throwing.
std::optional<helix::ams::SpoolEvidence> evidence_from_fingerprint(const std::string& fingerprint) {
    if (fingerprint.empty()) {
        return std::nullopt;
    }
    helix::ams::SpoolEvidence evidence;
    if (fingerprint == "-") {
        evidence.tag_read_complete = true;
        return evidence;
    }
    if (fingerprint.rfind("M|", 0) == 0) {
        // A tag whose UID never decoded: material and colour stand, and the
        // UID is the one part of the read still outstanding.
        const size_t split = fingerprint.find('|', 2);
        evidence.material = fingerprint.substr(2, split - 2);
        if (split != std::string::npos) {
            uint32_t color = 0;
            const std::string_view hex(fingerprint.data() + split + 1,
                                       fingerprint.size() - split - 1);
            const auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), color, 16);
            if (ec == std::errc{} && ptr == hex.data() + hex.size()) {
                evidence.color_rgb = color;
            }
        }
        return evidence;
    }
    evidence.tag_uid = fingerprint;
    evidence.tag_read_complete = true;
    return evidence;
}

} // namespace

// Status Update Handling
// ============================================================================

void AmsBackendSnapmaker::handle_status_update(const nlohmann::json& notification) {
    // notify_status_update format: {"method":"notify_status_update","params":[{...}, timestamp]}
    // Initial query responses send unwrapped status directly — handle both.
    const nlohmann::json* status_ptr = &notification;
    if (notification.contains("params") && notification["params"].is_array() &&
        !notification["params"].empty()) {
        status_ptr = &notification["params"][0];
    }
    const auto& status = *status_ptr;
    if (!status.is_object())
        return;

    bool changed = false;
    // Set when the active-tool port-present flag changed this parse (#991), so
    // we publish to AmsState exactly once after releasing the mutex.
    bool port_present_changed = false;
    // Lanes that reached "unload_finish" this parse. Same deferral rule as
    // port_present_changed: collected under mutex_, published to AmsState after
    // it is released, because reaching into AmsState while holding ours inverts
    // the order add_backend() acquires them in.
    std::vector<int> unloaded_lanes;
    // Channels whose feed-port presence rose this parse with no tag evidence
    // behind it. Same deferral rule as unloaded_lanes: the notice reaches
    // through AmsState and the UI queue, which must not run under mutex_.
    std::vector<int> unverified_insert_lanes;
    // The cursor head's *_fail state, when the active batch hit one this
    // parse. Same deferral rule as unloaded_lanes: end_firmware_batch() sends
    // gcode, which must not run under mutex_.
    int batch_failed_head = -1;
    std::string batch_failed_state;

    // What this parse physically read off each channel's spool. A default
    // entry (no UID, read not finished) means the notification carried no
    // filament_detect.info for that channel, which the insert rule reads as
    // no signal.
    std::array<helix::ams::SpoolEvidence, NUM_TOOLS> observed_evidence{};

    { // Scope lock — emit_event MUST be called outside mutex_ to avoid deadlock
      // with sync_from_backend() which acquires mutex_ via get_system_info()
        std::lock_guard<std::mutex> lock(mutex_);

        // Parse extruder0..3 state
        // Klipper uses "extruder" for T0, "extruder1" for T1, etc.
        static const std::string extruder_keys[] = {"extruder", "extruder1", "extruder2",
                                                    "extruder3"};
        for (int i = 0; i < NUM_TOOLS; i++) {
            const auto& key = extruder_keys[i];
            if (status.contains(key) && status[key].is_object()) {
                auto new_state = parse_extruder_state(status[key]);

                // Update slot status based on extruder state (only if pin state changed)
                auto* slot = system_info_.units[0].get_slot(i);
                if (slot) {
                    SlotStatus prev = slot->status;
                    if (new_state.active_pin) {
                        slot->status = SlotStatus::LOADED;
                    } else if (new_state.park_pin) {
                        slot->status = SlotStatus::AVAILABLE;
                    }
                    if (slot->status != prev)
                        changed = true;
                }

                extruder_states_[i] = std::move(new_state);
            }
        }

        // Detect active tool from extruder pin state and toolhead.extruder.
        // Only update when we have actual evidence — incremental status updates
        // may omit extruder/toolhead keys, so preserve the current value when
        // no relevant data is present (prevents oscillation between valid and -1).
        bool has_extruder_data = false;
        int active = -1;
        for (int i = 0; i < NUM_TOOLS; i++) {
            if (extruder_states_[i].active_pin ||
                (!extruder_states_[i].state.empty() && extruder_states_[i].state != "PARKED")) {
                active = i;
                has_extruder_data = true;
                break;
            }
        }
        if (status.contains("toolhead") && status["toolhead"].is_object()) {
            const auto& th = status["toolhead"];
            if (th.contains("extruder") && th["extruder"].is_string()) {
                auto ext_name = th["extruder"].get<std::string>();
                // "extruder" = 0, "extruder1" = 1, etc. An unparseable name
                // leaves whatever the per-extruder state loop above decided.
                if (const auto tool_number = helix::tool_number_for_extruder(ext_name)) {
                    active = *tool_number;
                }
                has_extruder_data = true;
            }
        }
        if (has_extruder_data && active != system_info_.current_tool) {
            // Demote previous active tool from LOADED to AVAILABLE
            if (system_info_.current_tool >= 0 && system_info_.current_tool < NUM_TOOLS) {
                auto* prev_slot = system_info_.units[0].get_slot(system_info_.current_tool);
                if (prev_slot && prev_slot->status == SlotStatus::LOADED) {
                    prev_slot->status = SlotStatus::AVAILABLE;
                }
            }
            system_info_.current_tool = active;
            system_info_.current_slot = active; // 1:1 tool-to-slot on Snapmaker
            system_info_.filament_loaded = (active >= 0);
            // Mark active tool's slot as LOADED
            if (active >= 0 && active < NUM_TOOLS) {
                auto* slot = system_info_.units[0].get_slot(active);
                if (slot && slot->status != SlotStatus::EMPTY) {
                    slot->status = SlotStatus::LOADED;
                }
            }
            changed = true;
        }

        // Parse filament_detect info (RFID data per channel)
        if (status.contains("filament_detect") && status["filament_detect"].is_object()) {
            const auto& fd = status["filament_detect"];

            // Parse RFID info per channel — filament_detect.info is a JSON array [ch0, ch1, ch2,
            // ch3] Only apply RFID data when it contains real values (not "NONE").
            // print_task_config is the authoritative source; RFID supplements it when tags are
            // present.
            if (fd.contains("info") && fd["info"].is_array()) {
                const auto& info_arr = fd["info"];
                for (int i = 0; i < NUM_TOOLS && i < static_cast<int>(info_arr.size()); i++) {
                    if (!info_arr[i].is_object())
                        continue;
                    auto rfid = parse_rfid_info(info_arr[i]);

                    // Capture what this reading physically got off the spool,
                    // before any early exit, so the insert rule sees it
                    // regardless of whether the rest of the RFID fields apply.
                    // A read is finished only when it named a tag: a UID names
                    // it, and a decoded MAIN_TYPE with no UID leaves the UID
                    // the one part still outstanding (material and colour
                    // stand as evidence without it). A NONE entry with no UID
                    // is three indistinguishable states - reader disabled,
                    // untagged spool, empty channel - so it files no evidence
                    // at all and the fingerprint comes out empty (no signal).
                    helix::ams::SpoolEvidence& evidence = observed_evidence[i];
                    evidence.tag_uid = rfid.uid;
                    if (!rfid.uid.empty()) {
                        evidence.tag_read_complete = true;
                    }
                    // A pending insert is judged HERE: this entry is the
                    // reader's answer for the spool that just went in, which
                    // the port edge could not know. A UID or a decoded
                    // MAIN_TYPE verifies it - the tail's
                    // check_hardware_event_clear judges any swap from this
                    // very reading - and an entry that files nothing is the
                    // reader saying no tag is behind the insert, so the stored
                    // record could describe a spool that left (#1710).
                    if (pending_insert_passes_[i] > 0) {
                        pending_insert_passes_[i] = 0;
                        if (rfid.uid.empty() && rfid.main_type == "NONE") {
                            unverified_insert_lanes.push_back(i);
                        }
                    }
                    if (rfid.main_type != "NONE") {
                        evidence.material = rfid.main_type;
                        if (helix::ams::is_declarable_color(rfid.color_rgb)) {
                            evidence.color_rgb = rfid.color_rgb;
                        }
                    }

                    // Skip entirely if RFID reader is disabled or no tag present
                    if (rfid.main_type == "NONE")
                        continue;

                    auto* slot = system_info_.units[0].get_slot(i);
                    if (slot) {
                        slot->material = rfid.main_type;
                        auto brand = !rfid.manufacturer.empty() ? rfid.manufacturer : rfid.vendor;
                        if (brand != "NONE")
                            slot->brand = brand;
                        slot->color_rgb = rfid.color_rgb;
                        // SUB_TYPE is Snapmaker's filament product-line name (e.g.
                        // "SnapSpeed" for their PLA line — akin to Polymaker's
                        // "PolyLite"). Maps to spool_name, NOT color_name. The
                        // Snapmaker RFID doesn't expose a dedicated color-name
                        // field — color_name stays unset here and is user-editable
                        // via the edit modal's color picker.
                        if (rfid.sub_type != "NONE")
                            slot->spool_name = rfid.sub_type;
                        slot->nozzle_temp_min = rfid.hotend_min_temp;
                        slot->nozzle_temp_max = rfid.hotend_max_temp;
                        slot->bed_temp = rfid.bed_temp;
                        slot->total_weight_g = static_cast<float>(rfid.weight_g);

                        // A tag read is a cache of what a vendor printed, not a
                        // sensor of identity: it survives the spool leaving the
                        // channel, so it never carries presence.
                        //
                        // Every value here is this parse's own, never slot->*.
                        // SlotInfo persists across frames and
                        // apply_resolved_lane rewrites it in place at the tail
                        // of every one, so
                        // reading the struct back would file a user's edit as
                        // something the tag says.
                        //
                        helix::ams::Observation cache(helix::ams::ObservationSource::VendorCache);
                        if (!rfid.main_type.empty())
                            cache.material = rfid.main_type;
                        // Both spellings of "the tag named no vendor" retract
                        // the brand here: whole-record replacement means this
                        // record states what THIS read said, and a field the
                        // read is silent about is not one it still stands
                        // behind. SlotInfo above tests only the literal "NONE",
                        // so it blanks on an absent key and KEEPS its last
                        // value on the literal. That one input is the only
                        // place the two layers disagree.
                        if (!brand.empty() && brand != "NONE")
                            cache.brand = brand;
                        // SnapmakerRfidInfo::color_rgb rests on
                        // AMS_DEFAULT_SLOT_COLOR when the tag carried no
                        // ARGB_COLOR, which is that struct's "no reading" and
                        // not a grey anybody chose.
                        if (helix::ams::is_declarable_color(rfid.color_rgb))
                            cache.color_rgb = rfid.color_rgb;
                        // SUB_TYPE names the product line inside MAIN_TYPE
                        // ("Silk" inside "PLA"), so it is the branded product
                        // and routing it to material would destroy the
                        // material. The SlotInfo field above keeps its own
                        // spelling, and splits from this record on the
                        // literal "NONE" for the same reason the brand guard
                        // does.
                        if (!rfid.sub_type.empty() && rfid.sub_type != "NONE")
                            cache.product_name = rfid.sub_type;
                        if (rfid.weight_g > 0)
                            cache.total_weight_g = static_cast<float>(rfid.weight_g);
                        // What this backend POSTed to filament_detect/set
                        // lands in this same object, spelled the same way, so
                        // a field repeating our own write is not a reading.
                        // Withholding it matters most AFTER the user clears
                        // their override: resolve() would otherwise fall
                        // through to a VendorCache record still holding the
                        // abandoned edit, and the lane could never get back to
                        // what the machine says. WEIGHT is nobody's
                        // declaration and passes through. The boundary rides
                        // the fingerprint spelling, the same one arm() takes
                        // from the tracker baseline, so a reading whose UID
                        // did not decode still names a spool boundary.
                        const int withheld = own_write_echoes_.withhold(
                            i, fingerprint_from_evidence(observed_evidence[i]), cache);
                        if (withheld > 0) {
                            spdlog::debug("{} Slot {} withheld {} field(s) echoing our own write",
                                          backend_log_tag(), i, withheld);
                        }
                        helix::ams::ingest(lane_id(i), cache);
                    }
                    changed = true;
                }
            }

            // Parse filament state per channel — filament_detect.state is
            // [int, int, int, int], the entrance/tag reader per channel. It
            // reads 0 once filament has been fed THROUGH it to the toolhead,
            // so a 0 is not "no filament": lane presence is declared at the
            // parse convergence point from the port sensor and the
            // loaded-at-toolhead latch, and this array only seeds a status
            // for slots nothing better has spoken for.
            if (fd.contains("state") && fd["state"].is_array()) {
                const auto& state_arr = fd["state"];
                for (int i = 0; i < NUM_TOOLS && i < static_cast<int>(state_arr.size()); i++) {
                    if (!state_arr[i].is_number())
                        continue;
                    int state_val = state_arr[i].get<int>();

                    auto* slot = system_info_.units[0].get_slot(i);
                    if (slot) {
                        // Only set from filament_detect if extruder state hasn't already
                        // provided a more authoritative status (LOADED/AVAILABLE via
                        // park_pin/active_pin)
                        if (slot->status == SlotStatus::UNKNOWN) {
                            slot->status =
                                (state_val != 0) ? SlotStatus::AVAILABLE : SlotStatus::EMPTY;
                        }
                        changed = true;
                    }
                }
            }
        }

        // Parse filament_feed left/right — top-level Klipper objects (not nested in
        // filament_detect) Each contains per-extruder state: filament_detected, channel_state,
        // channel_error
        int in_progress_head = -1;
        for (const auto& feed_key : {"filament_feed left", "filament_feed right"}) {
            if (status.contains(feed_key) && status[feed_key].is_object()) {
                const auto& feed = status[feed_key];
                for (int i = 0; i < NUM_TOOLS; i++) {
                    std::string ext_key = (i == 0) ? "extruder0" : fmt::format("extruder{}", i);
                    if (feed.contains(ext_key) && feed[ext_key].is_object()) {
                        const auto& ch = feed[ext_key];
                        // filament_detected: use .find() + is_boolean() (per
                        // [L087], matching the motion-sensor loop below) rather
                        // than .value(), which throws on the null Klipper
                        // publishes before the sensor's first reading. Because
                        // status frames are deltas, an omitted field means "no
                        // change" — treating it as false would clear the port
                        // sensor and drop the slot to EMPTY on a frame that
                        // said nothing about filament at all.
                        std::optional<bool> detected_opt;
                        auto fd_it = ch.find("filament_detected");
                        if (fd_it != ch.end() && fd_it->is_boolean()) {
                            const bool detected = fd_it->get<bool>();
                            detected_opt = detected;
                            // Mirror into port_sensor_filament_present_ so
                            // is_stuck_motion_sensor_runout can distinguish a real
                            // runout (both sensors false) from a stale motion-sensor
                            // false positive (motion=false, port=true). Tracked
                            // independent of slot->status because slot status flips
                            // to AVAILABLE/LOADED based on extruder pin state which
                            // is orthogonal to the port sensor reading.
                            if (i >= 0 && i < NUM_TOOLS) {
                                // The port flag's false -> true edge is an
                                // insert into the channel; the first sighting
                                // is the baseline, not an edge. The tag
                                // reader's answer lands a frame or two later,
                                // so the edge holds a pending verdict for the
                                // info loop to judge (#1710). A drop cancels
                                // one: the spool left before any read.
                                //
                                // A feed the firmware itself drives - its own
                                // tool-change unload/load, or one of our
                                // batch ops - drops and raises this flag too,
                                // and no spool changed hands, so it arms
                                // nothing. The edge runs before this frame's
                                // channel_state parse, so the gate reads the
                                // channel's last reported state; the state
                                // parse below cancels anything the ordering
                                // missed.
                                const bool firmware_driven =
                                    batch_.active || helix::snapmaker::channel_state_in_progress(
                                                         channel_snapshots_[i].state);
                                if (detected && feed_presence_seen_[i] &&
                                    !port_sensor_filament_present_[i] && !firmware_driven) {
                                    pending_insert_passes_[i] = 1;
                                } else if (!detected) {
                                    pending_insert_passes_[i] = 0;
                                }
                                port_sensor_filament_present_[i] = detected;
                                feed_presence_seen_[i] = true;
                            }
                            auto* slot = system_info_.units[0].get_slot(i);
                            if (slot) {
                                if (detected && (slot->status == SlotStatus::EMPTY ||
                                                 slot->status == SlotStatus::UNKNOWN)) {
                                    slot->status = SlotStatus::AVAILABLE;
                                    changed = true;
                                } else if (!detected && slot->status != SlotStatus::LOADED) {
                                    slot->status = SlotStatus::EMPTY;
                                    changed = true;
                                }
                            }
                        }

                        // Parse channel_state — the single authoritative signal
                        // for load state and operation progress. classify_channel_state
                        // maps every firmware state (39 total) to {action, phase,
                        // terminal, fail, latch set/clear}; the parse reads off that
                        // one table rather than scattered string compares. See
                        // u1_channel_state_reference.md.
                        // safe_string, not .value(): both fields are string-or-null
                        // on U1 firmware, and .value() throws on the null. The
                        // defaults below are already the intended "nothing to
                        // report" sentinels — "" is checked by the !state.empty()
                        // gate, "ok" by classify_channel_state.
                        auto state = helix::json_util::safe_string(ch, "channel_state", "");
                        auto error = helix::json_util::safe_string(ch, "channel_error", "ok");

                        // Keep the raw fields for eligibility queries; the
                        // latch below collapses them to a single bit.
                        // Status frames are deltas: start from the previous
                        // snapshot and overwrite only the keys this frame
                        // carries, so a channel_state-only frame leaves
                        // filament_detected/module_exist standing and a
                        // filament_detected-only frame does not blank state
                        // (the feeder frame never carries the motion sensor's
                        // enabled flag, so that field rides the same rule).
                        // Absent-or-null is "no change" for every field.
                        ChannelSnapshot snap = channel_snapshots_[static_cast<size_t>(i)];
                        const auto state_it = ch.find("channel_state");
                        if (state_it != ch.end() && state_it->is_string()) {
                            snap.state = state_it->get_ref<const std::string&>();
                        }
                        const auto error_it = ch.find("channel_error");
                        if (error_it != ch.end() && error_it->is_string()) {
                            snap.error = error_it->get_ref<const std::string&>();
                        }
                        if (detected_opt.has_value()) {
                            snap.filament_detected = *detected_opt;
                        }
                        const auto module_it = ch.find("module_exist");
                        if (module_it != ch.end() && module_it->is_boolean()) {
                            snap.module_exist = module_it->get<bool>();
                        }
                        const auto disable_it = ch.find("disable_auto");
                        if (disable_it != ch.end() && disable_it->is_boolean()) {
                            snap.disable_auto = disable_it->get<bool>();
                        }
                        channel_snapshots_[static_cast<size_t>(i)] = std::move(snap);

                        const ChannelStateInfo info = classify_channel_state(state);

                        // A feed under way on this channel is the firmware
                        // moving filament itself, so a presence edge that
                        // armed a pending verdict this parse was not a user
                        // insert; the spool never left.
                        if (pending_insert_passes_[i] > 0 &&
                            (info.action == AmsAction::LOADING ||
                             info.action == AmsAction::UNLOADING)) {
                            pending_insert_passes_[i] = 0;
                        }

                        // A channel reporting any state at all makes the
                        // lane's presence inputs live for the
                        // convergence-point ingest below.
                        if (!state.empty()) {
                            feed_presence_seen_[i] = true;
                        }

                        // Mirror the granular firmware sub-phase into the system
                        // info so the sidebar step bar can show the real
                        // Home/Select/Heat/Move sequence. -1 for any non-active
                        // state (idle, *_finish, *_fail, preload_finish). Updated
                        // only when the firmware actually reports a channel_state,
                        // so an incremental status omitting it doesn't clear the
                        // phase spuriously.
                        if (!state.empty()) {
                            if (system_info_.operation_phase != info.phase) {
                                system_info_.operation_phase = info.phase;
                                changed = true;
                            }
                        }

                        // Capture the head whose channel is mid-op; the single
                        // derivation of operation_working_slot below decides
                        // what the header names from it (batch cursor wins).
                        if (in_progress_head < 0 && (info.action == AmsAction::LOADING ||
                                                     info.action == AmsAction::UNLOADING)) {
                            in_progress_head = i;
                        }

                        // "Loaded at toolhead" latch (the core fix). Driven purely
                        // from channel_state transitions, NOT the motion sensor
                        // (which fails to clear after an unload on current firmware).
                        // SET on load_finish; CLEAR on unload_finish / wait_insert /
                        // preload_finish; KEEP on every transient / in-progress /
                        // fail state. Mirrors the firmware's persisted
                        // config['load_finish'].
                        if (!state.empty() && !info.ignore) {
                            if (info.sets_loaded && !loaded_at_toolhead_[i]) {
                                loaded_at_toolhead_[i] = true;
                                changed = true;
                            } else if (info.clears_loaded && loaded_at_toolhead_[i]) {
                                loaded_at_toolhead_[i] = false;
                                changed = true;
                            }
                        }

                        // Error surfacing: either a firmware channel_error token OR
                        // a *_fail channel_state (Change 2). Preserve the multi-color
                        // false-alarm guard — the firmware reports
                        // channel_error="no_filament" for ANY empty lane, and briefly
                        // a *_fail channel_state when it auto-feeds a lane deliberately
                        // left unloaded for a multi-color print (heads 0+2 used, head 1
                        // empty). Neither must latch the whole backend into
                        // action=Error and pop a spurious modal on such an idle empty
                        // non-active lane. An error is real when the lane holds
                        // filament (lane not empty), is the active lane, or an
                        // operation is genuinely underway on it (an in-progress
                        // LOADING/UNLOADING state — a *_fail is terminal, so the same
                        // empty-lane guard applies to it as to the no_filament token).
                        const bool has_error_token =
                            error != "ok" && !error.empty() && error != "none";
                        if (has_error_token || info.is_fail) {
                            const auto* slot = system_info_.units[0].get_slot(i);
                            const bool lane_empty = slot == nullptr || !slot->is_present();
                            const bool active_lane =
                                system_info_.current_slot == i || system_info_.current_tool == i;
                            const bool op_in_progress = info.action == AmsAction::LOADING ||
                                                        info.action == AmsAction::UNLOADING;
                            if (lane_empty && !op_in_progress && !active_lane) {
                                spdlog::debug(
                                    "[AmsBackendSnapmaker] ignoring error (token='{}' state='{}') "
                                    "on idle empty lane {} (not operating, not active)",
                                    error, state, i);
                            } else {
                                system_info_.action = AmsAction::ERROR;
                                system_info_.operation_detail =
                                    has_error_token
                                        ? friendly_channel_error(error, lane_noun(), i)
                                        : friendly_channel_state_fail(state, lane_noun(), i);
                                changed = true;
                            }
                        } else if (!state.empty() && !info.ignore) {
                            // No error — drive the action / operation lifecycle from
                            // the classifier.
                            if (info.action == AmsAction::LOADING) {
                                if (system_info_.action != AmsAction::LOADING) {
                                    system_info_.action = AmsAction::LOADING;
                                    changed = true;
                                }
                            } else if (info.action == AmsAction::UNLOADING) {
                                if (system_info_.action != AmsAction::UNLOADING) {
                                    system_info_.action = AmsAction::UNLOADING;
                                    changed = true;
                                }
                            } else if (info.is_terminal) {
                                // A *_finish state resolves the operation.
                                // unload_finish and preload_finish both retract
                                // filament out of the toolhead: demote the slot from
                                // LOADED to AVAILABLE and clear filament_loaded so
                                // slot_is_actively_loaded / filament_loaded clears
                                // immediately (the extruder pin-state path keeps
                                // active_pin set while parked, which otherwise leaves
                                // the badge "active" after an idle unload).
                                //
                                // current_slot / current_tool are NOT reset here:
                                // they track which toolhead is picked up on the
                                // carriage (toolhead.extruder is the authority, set
                                // in the extruder-pin parse above), which is
                                // independent of whether feeder filament is at the
                                // nozzle. A user running TPU without feeders (Bart's
                                // field report, 2026-07-20) has the tool picked up
                                // while the channel reports unload_finish
                                // permanently — resetting current_slot=-1 there
                                // made unload_active_filament() dispatch the bare
                                // INNER_FILAMENT_UNLOAD leaf macro (no tool
                                // specifier), and the firmware defaulted to T0.
                                // filament_loaded is the right signal for "no
                                // filament at the nozzle"; current_slot tracks the
                                // picked-up tool, full stop.
                                if (info.clears_loaded) {
                                    auto* slot = system_info_.units[0].get_slot(i);
                                    if (slot && slot->status == SlotStatus::LOADED) {
                                        slot->status = SlotStatus::AVAILABLE;
                                        changed = true;
                                    }
                                    if (system_info_.current_slot == i ||
                                        system_info_.current_tool == i) {
                                        system_info_.filament_loaded = false;
                                        changed = true;
                                    }
                                }
                                if (state == "unload_finish") {
                                    // Deferred to after the lock for the same
                                    // reason emit_event is: this reaches into
                                    // AmsState, which takes its own mutex, while
                                    // AmsState::add_backend() takes that mutex
                                    // first and then ours via set_event_callback().
                                    // Calling it here closed the cycle and TSan
                                    // reported the deadlock (nightly, 2026-08-16).
                                    unloaded_lanes.push_back(i);
                                }
                                // preload_finish is terminal-for-latch but does NOT
                                // end the op: a lane already at preload_finish that
                                // the user re-unloads keeps channel_state=preload_finish
                                // while the nozzle heats, and dropping to IDLE here
                                // killed the unload step display mid-heat
                                // (#u1-unload-steps). Only the true terminals resolve
                                // the action to IDLE.
                                if (state != "preload_finish") {
                                    if (system_info_.action == AmsAction::LOADING ||
                                        system_info_.action == AmsAction::UNLOADING) {
                                        system_info_.action = AmsAction::IDLE;
                                        system_info_.operation_detail.clear();
                                        PostOpCooldownManager::instance().schedule();
                                        changed = true;
                                    }
                                }
                            }
                            // IDLE non-terminal (none / inited / wait_insert): leave
                            // the action untouched — a stray idle mid-op must not
                            // clobber an in-progress LOADING/UNLOADING. The latch
                            // already handled wait_insert's clear above.
                        }

                        // Batch verification. Only the plan's cursor head can
                        // advance the cursor, so a sibling channel repeating its
                        // settled state in this frame is inert. The direction's
                        // own terminal is matched exactly: preload_finish and
                        // manual_sta_finish end a single-op lifecycle but a load
                        // batch counts a head only at load_finish (unload at
                        // unload_finish). A *_fail on the cursor head stops the
                        // batch where it stands; the error branch above has
                        // already set operation_detail to the failure message,
                        // which must win over a progress line. Runs after the
                        // terminal resolution so its operation_detail.clear()
                        // cannot wipe the progress string this writes.
                        if (batch_.active && i == batch_.heads[batch_.cursor]) {
                            if (info.is_fail) {
                                batch_.active = false;
                                batch_failed_head = i;
                                batch_failed_state = state;
                            } else if (state == (batch_.load ? "load_finish" : "unload_finish")) {
                                ++batch_.cursor;
                                batch_.active = batch_.cursor < batch_.heads.size();
                                if (batch_.active) {
                                    // "Load 2 of 4" — the head now in progress.
                                    // The words arrive pretranslated from
                                    // dispatch (main thread); this parse runs
                                    // on the WebSocket thread, which must not
                                    // call lv_tr.
                                    system_info_.operation_detail = fmt::format(
                                        "{} {} {} {}", batch_.direction_label, batch_.cursor + 1,
                                        batch_.of_label, batch_.heads.size());
                                } else {
                                    // Every head verified. Nothing is in
                                    // progress, and no later frame clears the
                                    // line once the action is IDLE.
                                    system_info_.operation_detail.clear();
                                }
                                changed = true;
                            }
                        }

                        // Diagnostic: trace the firmware channel_state sequence during
                        // a load/unload so we can tell which event is the TRUE physical
                        // completion vs an intermediate (preload_finish staged-in-buffer).
                        // The on-screen step bar / status was dropping to Idle before the
                        // physical unload finished; the real event order is firmware-
                        // specific and was previously unlogged. (#u1-unload-steps)
                        if (!state.empty()) {
                            spdlog::debug("[AmsBackendSnapmaker] tool {} channel_state='{}' "
                                          "error='{}' -> action={} current_slot={}",
                                          i, state, error,
                                          ams_action_to_string(system_info_.action),
                                          system_info_.current_slot);
                        }
                    }
                }
            }
        }

        // The batch macro's `doing` save-variable is the firmware's own word
        // on whether a batch script is running. A false reading retires any
        // plan this process still holds active: the script ended without the
        // cursor head reaching a terminal or a *_fail (lost response, script
        // abort, a feeder wedging mid-feed), and no channel_state detector
        // covers that end.
        bool batch_retired = false;
        if (!batch_macro_object_.empty()) {
            const auto macro = status.find(batch_macro_object_);
            if (macro != status.end() && macro->is_object()) {
                const auto doing = macro->find("doing");
                if (doing != macro->end() && doing->is_boolean() && !doing->get<bool>() &&
                    batch_.active) {
                    batch_.active = false;
                    batch_retired = true;
                    changed = true;
                    spdlog::info("{} batch macro reports doing=false — retiring the active plan",
                                 backend_log_tag());
                }
            }
        }

        // ONE derivation of "the head an operation is working on": the batch
        // cursor while a plan is active, else the head whose channel reported
        // an in-progress state, else none. A toolhead-only delta carries no
        // channel evidence, so mid-op it keeps the previous answer instead of
        // flapping the header back to the carriage tool; a batch the firmware
        // just reported ended carries nothing forward. current_slot is NOT
        // touched here: it stays the carriage answer its other consumers
        // (bypass unload, filament panel gating, the loaded card) read.
        int working_slot = -1;
        if (batch_.active) {
            working_slot = batch_.heads[batch_.cursor];
        } else if (system_info_.action == AmsAction::LOADING ||
                   system_info_.action == AmsAction::UNLOADING) {
            if (in_progress_head >= 0) {
                working_slot = in_progress_head;
            } else if (!batch_retired) {
                working_slot = system_info_.operation_working_slot;
            }
        }
        if (system_info_.operation_working_slot != working_slot) {
            system_info_.operation_working_slot = working_slot;
            changed = true;
        }

        // Parse print_task_config — authoritative filament info from Snapmaker's task manager
        // Contains per-extruder filament type, vendor, color, and presence data
        if (status.contains("print_task_config") && status["print_task_config"].is_object()) {
            const auto& ptc = status["print_task_config"];

            // Firmware-stored preferences. Merged field by field, because a delta
            // frame that mentions one setting says nothing about the others.
            // Held as told, never filed as lane observations — these are a write
            // surface like the filament_type/vendor/color fields below.
            const auto incoming = snapmaker::read_print_preferences(status);
            if (incoming.auto_replenish) {
                print_preferences_.auto_replenish = incoming.auto_replenish;
            }
            if (incoming.replenish_ignore_color) {
                print_preferences_.replenish_ignore_color = incoming.replenish_ignore_color;
            }
            if (incoming.filament_entangle_detect) {
                print_preferences_.filament_entangle_detect = incoming.filament_entangle_detect;
            }
            if (incoming.end_led_turn_off) {
                print_preferences_.end_led_turn_off = incoming.end_led_turn_off;
            }
            if (incoming.filament_entangle_sen) {
                print_preferences_.filament_entangle_sen = incoming.filament_entangle_sen;
            }
            if (!incoming.end_unload_filament.empty()) {
                print_preferences_.end_unload_filament = incoming.end_unload_filament;
            }

            // extruder_map_table: [int x32] — logical tool -> physical head. The
            // firmware's own routing authority for the running print (see the
            // member's doc comment). Mirrored verbatim; interpretation belongs to
            // get_tool_mapping()'s callers, not here.
            if (ptc.contains("extruder_map_table") && ptc["extruder_map_table"].is_array()) {
                std::vector<int> table;
                table.reserve(ptc["extruder_map_table"].size());
                for (const auto& entry : ptc["extruder_map_table"]) {
                    // A non-integer or out-of-range head is recorded as -1 ("no
                    // opinion") rather than clamped: silently substituting head 0
                    // is the identity-as-truth mistake this whole path exists to
                    // stop making.
                    if (!entry.is_number_integer()) {
                        table.push_back(-1);
                        continue;
                    }
                    const int head = entry.get<int>();
                    table.push_back((head >= 0 && head < NUM_TOOLS) ? head : -1);
                }
                if (table != extruder_map_table_) {
                    spdlog::debug("[AMS Snapmaker] extruder_map_table changed ({} entries)",
                                  table.size());
                    extruder_map_table_ = std::move(table);
                    changed = true;
                }
            }

            // extruders_used: [bool x4] — heads this task uses. Gates whether the
            // map above may be read at all (see the member's doc comment).
            if (ptc.contains("extruders_used") && ptc["extruders_used"].is_array()) {
                std::vector<bool> used;
                used.reserve(ptc["extruders_used"].size());
                for (const auto& entry : ptc["extruders_used"]) {
                    used.push_back(entry.is_boolean() && entry.get<bool>());
                }
                if (used != extruders_used_) {
                    extruders_used_ = std::move(used);
                    changed = true;
                }
            }

            // Snapshot the routing while the task is still configured. Both
            // fields are members, so this is evaluated against the accumulated
            // state rather than only what THIS frame carried — an incremental
            // update that names one of them still lands on the right answer.
            //
            // This is the only moment the routing is knowable. Once the print
            // ends the firmware clears extruders_used and resets the table, and a
            // reprint has nothing left to read: no detail view, no picker, no
            // colour match to recompute. An empty table is never snapshotted —
            // "known: nothing" is indistinguishable from a real answer to the
            // caller, and the honest value is "not known".
            const bool task_configured_now = std::any_of(
                extruders_used_.begin(), extruders_used_.end(), [](bool b) { return b; });
            if (task_configured_now && !extruder_map_table_.empty() &&
                last_task_extruder_map_ != extruder_map_table_) {
                last_task_extruder_map_ = extruder_map_table_;
                spdlog::debug("[AMS Snapmaker] recorded task routing ({} entries) for reprint",
                              last_task_extruder_map_.size());
            }

            // filament_exist: [bool, bool, bool, bool] — whether filament is loaded per slot
            if (ptc.contains("filament_exist") && ptc["filament_exist"].is_array()) {
                const auto& exist_arr = ptc["filament_exist"];
                for (int i = 0; i < NUM_TOOLS && i < static_cast<int>(exist_arr.size()); i++) {
                    if (!exist_arr[i].is_boolean())
                        continue;
                    bool exists = exist_arr[i].get<bool>();
                    auto* slot = system_info_.units[0].get_slot(i);
                    if (slot) {
                        if (exists && slot->status != SlotStatus::LOADED) {
                            slot->status = SlotStatus::AVAILABLE;
                        } else if (!exists) {
                            slot->status = SlotStatus::EMPTY;
                        }
                        changed = true;
                    }
                }
            }

            // These three fields write SlotInfo and deliberately file NO lane
            // observation, unlike the RFID parse above.
            //
            // print_task_config is a write surface, not a sensor.
            // SET_PRINT_FILAMENT_CONFIG takes VENDOR / FILAMENT_TYPE /
            // FILAMENT_SUBTYPE / FILAMENT_COLOR_RGBA as gcode parameters and
            // persists them, so whoever sent that command set these values: the
            // machine's own screen, a slicer, a console, or this backend's
            // write-back through /printer/filament_detect/set, which firmware
            // mirrors into this same struct. Filing any of it as VendorCache
            // would return a user's own edit as firmware truth.
            //
            // The firmware carries the provenance bit itself, and it shows the
            // channel is redundant rather than merely unsafe: filament_official
            // marks a head whose entry came from a Snapmaker RFID spool, and
            // SET_PRINT_FILAMENT_CONFIG is refused on such a head without
            // FORCE. An official entry is the tag filament_detect.info already
            // reports, which the RFID parse files; an unofficial one is
            // somebody's declaration. Neither is a reading this key can
            // contribute.
            //
            // A user's declaration reaches the lane model through
            // commit_slot_edit, which is the funnel that records authorship.
            //
            // filament_type: ["PLA", "PLA", ...] — material type per slot
            if (ptc.contains("filament_type") && ptc["filament_type"].is_array()) {
                const auto& type_arr = ptc["filament_type"];
                for (int i = 0; i < NUM_TOOLS && i < static_cast<int>(type_arr.size()); i++) {
                    if (!type_arr[i].is_string())
                        continue;
                    auto* slot = system_info_.units[0].get_slot(i);
                    if (slot) {
                        auto type = type_arr[i].get<std::string>();
                        slot->material = type; // Base type only (e.g., "PLA") for compact display
                        changed = true;
                    }
                }
            }

            // filament_vendor: ["Snapmaker", ...] — brand per slot
            if (ptc.contains("filament_vendor") && ptc["filament_vendor"].is_array()) {
                const auto& vendor_arr = ptc["filament_vendor"];
                for (int i = 0; i < NUM_TOOLS && i < static_cast<int>(vendor_arr.size()); i++) {
                    if (!vendor_arr[i].is_string())
                        continue;
                    auto* slot = system_info_.units[0].get_slot(i);
                    if (slot) {
                        slot->brand = vendor_arr[i].get<std::string>();
                        changed = true;
                    }
                }
            }

            // filament_color_rgba: ["080A0DFF", "E2DEDBFF", ...] — hex RGBA color per slot
            if (ptc.contains("filament_color_rgba") && ptc["filament_color_rgba"].is_array()) {
                const auto& color_arr = ptc["filament_color_rgba"];
                for (int i = 0; i < NUM_TOOLS && i < static_cast<int>(color_arr.size()); i++) {
                    if (!color_arr[i].is_string())
                        continue;
                    auto* slot = system_info_.units[0].get_slot(i);
                    if (slot) {
                        auto hex = color_arr[i].get<std::string>();
                        // RGBA hex string → RGB uint32: take first 6 chars
                        if (hex.size() >= 6) {
                            try {
                                slot->color_rgb = std::stoul(hex.substr(0, 6), nullptr, 16);
                            } catch (...) {
                            }
                        }
                        changed = true;
                    }
                }
            }
        }

        // Parse filament_motion_sensor / filament_switch_sensor for per-slot
        // runout state. Snapmaker U1's config has [filament_motion_sensor e{N}_filament]
        // with pause_on_runout=True; when filament stops moving past the encoder,
        // Klipper publishes filament_detected:false and triggers PAUSE. The slot
        // status / extruder pin state don't reflect this — the tool is still
        // "active" but no filament reaches the nozzle. Mirror the sensor flag so
        // the path canvas can break the spool→toolhead line at runout.
        //
        // Match both prefixes (motion is the Snapmaker default; switch is the
        // generic fallback) and any "e{N}_filament" / "e{N}" sensor name suffix.
        for (auto it = status.begin(); it != status.end(); ++it) {
            const std::string& key = it.key();
            const auto motion_prefix = std::string_view("filament_motion_sensor ");
            const auto switch_prefix = std::string_view("filament_switch_sensor ");
            std::string_view sensor_name;
            if (key.compare(0, motion_prefix.size(), motion_prefix) == 0) {
                sensor_name = std::string_view(key).substr(motion_prefix.size());
            } else if (key.compare(0, switch_prefix.size(), switch_prefix) == 0) {
                sensor_name = std::string_view(key).substr(switch_prefix.size());
            } else {
                continue;
            }
            // Expect "e{N}_filament" or "e{N}". Anything else (toolhead_sensor,
            // bypass_sensor, custom names) is unrelated to per-tool runout.
            if (sensor_name.size() < 2 || sensor_name[0] != 'e')
                continue;
            int tool_idx = -1;
            try {
                size_t digit_end = 1;
                while (digit_end < sensor_name.size() &&
                       std::isdigit(static_cast<unsigned char>(sensor_name[digit_end]))) {
                    ++digit_end;
                }
                if (digit_end == 1)
                    continue; // no digits
                tool_idx = std::stoi(std::string(sensor_name.substr(1, digit_end - 1)));
            } catch (...) {
                continue;
            }
            if (tool_idx < 0 || tool_idx >= NUM_TOOLS)
                continue;
            if (!it.value().is_object())
                continue;
            // `enabled` rides the same status objects and gates loading: a
            // sensor the firmware has disabled cannot confirm feed. Absent
            // means no change (delta frames omit held values).
            auto enabled_it = it.value().find("enabled");
            if (enabled_it != it.value().end() && enabled_it->is_boolean()) {
                channel_snapshots_[static_cast<size_t>(tool_idx)].sensor_enabled =
                    enabled_it->get<bool>();
            }
            // filament_detected: Klipper emits as bool; default true (no runout)
            // so missing field == "no change" via the contains check. Use .find()
            // + is_boolean() (per [L087]) rather than .value() which would throw
            // on a null payload.
            auto fd_it = it.value().find("filament_detected");
            if (fd_it == it.value().end() || !fd_it->is_boolean())
                continue;
            bool present = fd_it->get<bool>();
            if (sensor_filament_present_[tool_idx] != present) {
                sensor_filament_present_[tool_idx] = present;
                changed = true;
                spdlog::info("{} Tool {} filament sensor: {} ({})", backend_log_tag(), tool_idx,
                             present ? "PRESENT" : "RUNOUT", key);
            }
        }

        // If the active tool's filament sensor reports runout, the global
        // filament_loaded flag (used by get_filament_segment) should reflect that.
        // The pin-state path above sets filament_loaded=(active>=0) — override
        // here so the canvas's spool→toolhead line breaks on runout even though
        // the tool itself is still "active".
        if (system_info_.current_tool >= 0 && system_info_.current_tool < NUM_TOOLS &&
            !sensor_filament_present_[system_info_.current_tool]) {
            if (system_info_.filament_loaded) {
                system_info_.filament_loaded = false;
                changed = true;
            }
        }

        // Per-slot runout demotion: any slot whose motion sensor reports
        // no filament should be AVAILABLE (spool present, ready to feed), not
        // LOADED. Without this, the AMS context menu's Load button is gated off
        // (pending_is_loaded_ from slot.status==LOADED disables it) and the user
        // has no way to re-feed filament from the UI after a runout — they get
        // Unload/Reset on a slot that has no filament between feeder and nozzle.
        // EMPTY is wrong here because the slot's RFID/print_task_config still
        // reports a spool present; AVAILABLE accurately captures "spool yes,
        // filament-at-toolhead no".
        for (int i = 0; i < NUM_TOOLS; ++i) {
            if (sensor_filament_present_[i])
                continue;
            auto* slot = system_info_.units[0].get_slot(i);
            if (slot && slot->status == SlotStatus::LOADED) {
                slot->status = SlotStatus::AVAILABLE;
                changed = true;
            }
        }

        // Parse convergence point. After every firmware-sourced field on the
        // SlotInfo has been populated above, loop through slots and lay each
        // lane's resolved values on top. check_hardware_event_clear must run
        // FIRST so it sees firmware-truth fields (not the resolved view) and
        // can clear a stale override when a physical spool swap is detected.
        // apply_resolved_lane runs after, so the final SlotInfo the UI reads
        // through get_slot_info / the emitted event reflects what the lane
        // resolves to.
        //
        // Snapmaker has multiple parse paths feeding the same slot (RFID info,
        // print_task_config, filament_feed). Rather than hook the override logic
        // into each one, we run it once here at the tail — the tradeoff is that
        // get_slot_info during a partial parse would observe uncleared overrides,
        // but since everything runs under mutex_ and handle_status_update is the
        // only writer, there's no observable window.
        for (int i = 0; i < NUM_TOOLS; ++i) {
            auto* slot = system_info_.units[0].get_slot(i);
            if (!slot)
                continue;

            // A pending insert ages one pass per parse. A read that never
            // lands (reader disabled, the channel's entry never came) must
            // not hold its verdict forever: past the bound, ask (#1710).
            if (pending_insert_passes_[i] > 0 &&
                ++pending_insert_passes_[i] > kSnapPendingInsertPasses) {
                pending_insert_passes_[i] = 0;
                unverified_insert_lanes.push_back(i);
            }

            // A channel this parse carried no filament_detect.info for keeps
            // its default evidence, which the insert rule reads as no signal -
            // so the call is unconditional rather than gated on which keys the
            // notification happened to carry.
            check_hardware_event_clear(*slot, i, observed_evidence[i]);
            // Mirror firmware-truth color/material into lane_data so OrcaSlicer's
            // MoonrakerPrinterAgent sees the spool. OverwriteAlways policy: user
            // edits via apply_user_edit round-trip through firmware via the
            // POST /printer/filament_detect/set endpoint (paxx12 Extended Firmware),
            // so firmware-truth and user-truth converge, and overwriting lane_data
            // is safe and also catches external edits (CHANGE_ZCOLOR
            // from a print, manual gcode, OrcaSlicer, etc). On stock firmware the
            // POST 404s, but the override is still persisted to lane_data
            // separately, so this overwrite is the only path that could theoretically
            // de-sync — accept that tradeoff in exchange for picking up external
            // edits on extension-enabled firmware. See mirror_firmware_to_lane_data
            // docs and AD5X IFS for the same pattern.
            //
            // The stored override defers to what a declaring lane source holds,
            // so the store reads the lane here. A Spoolman record or a user's
            // value never reaches firmware, and firmware's reading must not
            // overwrite it in the override or in the lane_data record it
            // persists.
            helix::ams::mirror_firmware_to_lane_data(
                override_store_.get(), overrides_, i, slot->color_rgb, slot->material,
                slot->status == SlotStatus::AVAILABLE, helix::ams::MirrorPolicy::OverwriteAlways,
                backend_log_tag(), helix::ams::declared_on_lane(lane_id(i)));

            // Lane presence: the port/buffer sensor OR the loaded-at-toolhead
            // latch. The port sensor is the spool-side reading; the latch
            // carries filament fed through to the nozzle — the point at which
            // the entrance/tag reader (filament_detect.state) drops to 0, so
            // that array is not a presence source. Declared here, at the
            // tail of the parse, so both member arrays already hold this
            // frame's values when the lane resolves below; the arrays persist
            // across delta frames, so a frame silent on both signals leaves
            // the last reading standing. A lane filament_feed has never
            // reported stays silent too: the array defaults are "no reading
            // yet", not "no filament".
            if (feed_presence_seen_[i]) {
                helix::ams::Observation sensed(helix::ams::ObservationSource::Sensed);
                sensed.present = port_sensor_filament_present_[i] || loaded_at_toolhead_[i];
                helix::ams::ingest(lane_id(i), sensed);
            }

            apply_resolved_lane(*slot, i);
        }

        // First-gate (port) filament presence for the ACTIVE tool (#991). The
        // runout dialog gates Resume on THIS signal — the port/buffer sensor that
        // flips true the moment a user re-feeds a spool — NOT the toolhead motion
        // sensor (sensor_filament_present_), which stays "runout" until extrusion.
        // No active tool → treat as present (1) so Resume is never gated. Computed
        // under mutex_ (reads current_tool + the port array); published after the
        // mutex is released. Only publish on an actual change to avoid spamming
        // the UpdateQueue on every incremental notify.
        int active_tool = system_info_.current_tool;
        bool active_port_present = !(active_tool >= 0 && active_tool < NUM_TOOLS) ||
                                   port_sensor_filament_present_[active_tool];
        int port_val = active_port_present ? 1 : 0;
        if (port_val != last_published_port_present_) {
            last_published_port_present_ = port_val;
            port_present_changed = true;
        }

    } // Release mutex_ before emitting event

    if (port_present_changed) {
        AmsState::instance().set_active_tool_port_present(last_published_port_present_ != 0);
    }

    // Record the just-unloaded lanes so FilamentSensorManager suppresses the
    // runout modal during the grace window when the user is EXPECTED to pull
    // filament out of the lane.
    for (int lane : unloaded_lanes) {
        AmsState::instance().mark_slot_unloaded(lane);
    }

    // An insert the RFID side vouches nothing for asks whether the stored
    // record still describes the spool that went in (#1710). The notice
    // re-checks its own guards (print-feeding lane, lane with nothing to
    // clear) on the UI thread.
    for (int lane : unverified_insert_lanes) {
        helix::ui::queue_update([lane] { helix::ui::offer_clear_after_unverified_insert(lane); });
    }

    if (batch_failed_head >= 0) {
        spdlog::warn("{} head {} reached '{}' — clearing the firmware batch interlock",
                     backend_log_tag(), batch_failed_head, batch_failed_state);
        end_firmware_batch();
    }

    if (changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

// ============================================================================
// Override layering
// ============================================================================

void AmsBackendSnapmaker::check_hardware_event_clear(SlotInfo& slot, int slot_index,
                                                     const helix::ams::SpoolEvidence& observed) {
    // The bookkeeping lives in the shared tracker (CFS runs the same one);
    // the verdict on a change is the insert rule's. Snapmaker registers no
    // expect() value, so OwnWriteEcho cannot occur here - nothing on this
    // backend writes a CARD_UID back to firmware.
    //
    //   NoSignal  = the frame answered no RFID info for the channel. Baseline
    //               untouched, no clear.
    //   Baseline  = first reading. Even when the override was saved against a
    //               different spool, the first observation is NEVER a swap
    //               signal; apply_resolved_lane runs after us and a declared
    //               value outranks this reading.
    //   Unchanged = the same reading repeated.
    std::string old_fingerprint;
    const auto event =
        rfid_tracker_.observe(slot_index, fingerprint_from_evidence(observed), &old_fingerprint);
    if (event != helix::ams::FingerprintEvent::Changed) {
        if (event == helix::ams::FingerprintEvent::Baseline) {
            spdlog::debug("{} Slot {} baseline RFID fingerprint: {}", backend_log_tag(), slot_index,
                          fingerprint_from_evidence(observed));
        }
        return;
    }

    const auto verdict =
        helix::ams::classify_insert(evidence_from_fingerprint(old_fingerprint), observed);
    if (verdict != helix::ams::InsertVerdict::DifferentSpool) {
        spdlog::debug(
            "{} Slot {} RFID fingerprint changed {} -> {} ({}); record stands", backend_log_tag(),
            slot_index, old_fingerprint, fingerprint_from_evidence(observed),
            verdict == helix::ams::InsertVerdict::SameSpool ? "same spool" : "no evidence");
        return;
    }

    auto ovr_it = overrides_.find(slot_index);
    if (ovr_it == overrides_.end()) {
        spdlog::debug("{} Slot {} RFID fingerprint changed {} -> {} (no override to clear)",
                      backend_log_tag(), slot_index, old_fingerprint,
                      fingerprint_from_evidence(observed));
        return;
    }

    spdlog::info("{} Slot {} RFID fingerprint changed {} -> {}, clearing override "
                 "(different spool detected)",
                 backend_log_tag(), slot_index, old_fingerprint,
                 fingerprint_from_evidence(observed));

    // Delegate the erase + field reset + clear_async to the shared helper so
    // hardware-event clears and user-initiated clears share one field-reset
    // policy. Caller already holds mutex_.
    (void)ovr_it; // erased inside clear_override_locked
    clear_override_locked(slot_index, slot);
}

void AmsBackendSnapmaker::clear_override_locked(int slot_index, SlotInfo& slot) {
    // Caller must hold mutex_. Erases the in-memory override, resets STRICTLY
    // override-exclusive fields on the live SlotInfo so the cleared state is
    // visible in the very next get_slot_info() read.
    //
    // Snapmaker field policy: brand / spool_name / total_weight_g come from
    // the RFID tag in handle_status_update — we must NOT zero those here or
    // we'd wipe newly-parsed firmware metadata. The override's copies of
    // those fields disappear with the erase; firmware's copies stay.
    // (color_name is not firmware-populated for Snapmaker — RFID has no
    // color-name field — so it's override-exclusive and gets cleared.)
    overrides_.erase(slot_index);
    // The lane's own records go with it: the erase above and this are one
    // clear in two stores, and a clear that reached only one would leave
    // resolve() still reporting the identity just removed.
    helix::ams::reset_lane_to_machine_readings(lane_id(slot_index));
    // The echo guard goes with them: it was suspending readings of an
    // identity this clear just removed, on a lane whose next frame is the
    // machine's own state. Covers both callers - the Clear Spool gesture and
    // the RFID swap, whose differing tag would disarm at withhold() anyway.
    own_write_echoes_.abandon(slot_index);

    // All three Spoolman handles die with the override. The full
    // SlotInfo::clear_spoolman_link() is withheld here: it also zeroes
    // spool_name, which Snapmaker RFID firmware owns and re-supplies.
    slot.spoolman_id = 0;
    slot.spoolman_vendor_id = 0;
    slot.spoolman_filament_id = 0;
    slot.remaining_weight_g = -1.0f;
    slot.color_name.clear();
    // The catalog pick is override-exclusive on every backend — no AMS
    // firmware carries a branded product id — so a clear always drops it.
    // Leaving it would re-navigate the editor to the removed spool's
    // product on the next open.
    slot.catalog_id.clear();
    slot.product_name.clear();

    if (override_store_) {
        // Capture by value only — clear_async's Moonraker callback can fire
        // after this function returns (MR tracker ~60s) and potentially
        // after the backend itself is gone. Same rationale as save_async.
        const std::string tag = backend_log_tag();
        override_store_->clear_async(slot_index, [tag, slot_index](bool ok, std::string err) {
            if (!ok) {
                spdlog::warn("{} clear_async failed for slot {}: {}", tag, slot_index, err);
            }
        });
    }
}

void AmsBackendSnapmaker::clear_slot_override(int slot_index) {
    if (auto err = validate_slot_index(slot_index); !err.success()) {
        spdlog::warn("{} clear_slot_override: invalid slot {}", backend_log_tag(), slot_index);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot =
            system_info_.units.empty() ? nullptr : system_info_.units[0].get_slot(slot_index);
        if (!slot) {
            spdlog::warn("{} clear_slot_override: no slot entry for index {}", backend_log_tag(),
                         slot_index);
            return;
        }
        spdlog::info("{} Slot {} override cleared by user request", backend_log_tag(), slot_index);
        clear_override_locked(slot_index, *slot);
    }

    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
}

// ============================================================================
// Internal Helpers
// ============================================================================

std::vector<int> AmsBackendSnapmaker::task_routing(const std::vector<bool>& extruders_used,
                                                   const std::vector<int>& extruder_map) {
    // Gate on a task actually being configured. With no task the firmware holds
    // a default identity map, and answering [0,1,2,3] there would hand callers a
    // confident wrong routing for any file whose tools do not line up with the
    // lanes — the identity-as-truth mistake this accessor exists to end, just
    // moved one layer down. All-false extruders_used is the firmware's own "no
    // task" signal: it sets the flags when a task is set up and clears them when
    // the print ends (observed idle [F,F,F,F], mid-print [F,F,T,F]).
    const bool task_configured =
        std::any_of(extruders_used.begin(), extruders_used.end(), [](bool b) { return b; });
    if (!task_configured) {
        return {};
    }
    return extruder_map;
}

std::vector<int> AmsBackendSnapmaker::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return task_routing(extruders_used_, extruder_map_table_);
}

std::vector<int> AmsBackendSnapmaker::last_print_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Deliberately NOT gated on a task being configured: the whole point is to
    // answer after the task has ended, which is when a reprint asks. Empty means
    // no configured task has ever been observed.
    return last_task_extruder_map_;
}

std::string AmsBackendSnapmaker::build_preprint_gcode(const std::set<int>& tools_used,
                                                      const std::map<int, int>& remap) const {
    return preprint_gcode(tools_used, remap);
}

std::string AmsBackendSnapmaker::preprint_gcode(const std::set<int>& tools_used,
                                                const std::map<int, int>& remap) {
    if (tools_used.empty()) {
        return "";
    }

    // Ask the backend's own routing table rather than restating it here.
    // FilamentMapper::identity_filtered_remap() decides which mappings count as
    // genuine remaps from this same table; a second copy of it would let the two
    // halves disagree about which head a tool defaults to, filtering a mapping
    // out as identity while resolving it somewhere else.
    const helix::FirmwareRouting routing = default_routing();

    // Resolve every used logical tool to the head it must print from: the user's
    // remap when there is one, the firmware default otherwise. A tool the routing
    // gives no head (-1) is dropped: MAP_EXTRUDER=-1 clears the firmware's
    // `>= PHYSICAL_EXTRUDER_NUM` bounds check and then indexes
    // extruder_map_table[-1], which in Python is the LAST entry.
    std::map<int, int> resolved;
    for (int t : tools_used) {
        auto it = remap.find(t);
        const int head = (it != remap.end()) ? it->second : routing.head(t);
        if (head < 0) {
            spdlog::warn("[Snapmaker] preprint: tool {} has no head in the firmware routing - "
                         "leaving it out of the extruder map",
                         t);
            continue;
        }
        resolved[t] = head;
    }
    if (resolved.empty()) {
        return "";
    }

    std::vector<std::string> lines;

    // Emit SET_PRINT_EXTRUDER_MAP for EVERY used tool, including the ones landing
    // on their firmware-default head.
    //
    // Emitting only the genuine remaps left the rest at whatever the PREVIOUS
    // print wrote: the command sets one entry and resets nothing, and the
    // firmware's reset_print_info() does not run between our send and the print.
    // So a job that remapped T0->head2 left extruder_map_table[0]=2 behind, and
    // the next job — needing plain T0->head0 and therefore emitting nothing for
    // it — printed from head 2. That also corrupted the SET_PRINT_USED_EXTRUDERS
    // line below, which assumed the default applied. Writing each used entry
    // explicitly makes the table say exactly what this print means, which is
    // also what lets get_tool_mapping() be read back as truth.
    //
    // std::map iterates in ascending key order, so the sequence is deterministic.
    for (const auto& [logical, physical] : resolved) {
        lines.push_back(fmt::format("SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER={} MAP_EXTRUDER={}",
                                    logical, physical));
    }

    // Derived from the SAME resolution, so the two commands cannot disagree.
    std::set<int> used_heads;
    for (const auto& [logical, physical] : resolved) {
        (void)logical;
        used_heads.insert(physical);
    }

    std::string csv;
    for (int head : used_heads) {
        if (!csv.empty()) {
            csv += ',';
        }
        csv += std::to_string(head);
    }
    lines.push_back(fmt::format("SET_PRINT_USED_EXTRUDERS EXTRUDERS={}", csv));

    // Join with newlines, no trailing newline.
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out += '\n';
        }
        out += lines[i];
    }
    return out;
}

// The U1 blocks RESUME whenever a used extruder still reads filament_type
// "" or "NONE" in print_task_config: INNER_CHECK_AND_RELOAD_FILAMENT_INFO
// raises `e<N> not edit filament`. Tagless third-party spools land here,
// because only a Snapmaker RFID spool fills that field on its own.
//
// The refusal is raised oneshot, so it never reaches print_stats.exception --
// that still holds whatever paused the print, typically a runout. The generic
// classifier would see an uncoded `!!` on a paused printer and offer Resume,
// which this fault refuses again, redisplaying the same modal indefinitely.
std::optional<helix::ErrorEvent>
AmsBackendSnapmaker::classify_error(const std::string& raw_line,
                                    const helix::ClassifyContext& ctx) const {
    if (!helix::is_bang_line(raw_line)) {
        return std::nullopt;
    }

    // Only meaningful against a job the user is trying to continue. Echoing the
    // words into the console on an idle machine earns no modal.
    if (!ctx.is_paused && !ctx.is_printing) {
        return std::nullopt;
    }

    // Klipper's wording, verbatim: "e<N> not edit filament".
    const std::string detail = helix::strip_bang_prefix(raw_line);
    static constexpr std::string_view kSuffix = " not edit filament";
    if (detail.empty() || detail.front() != 'e') {
        return std::nullopt;
    }
    const auto suffix_pos = detail.find(kSuffix);
    if (suffix_pos == std::string::npos || suffix_pos < 2) {
        return std::nullopt;
    }
    const std::string digits = detail.substr(1, suffix_pos - 1);
    if (!is_parsable_index(digits)) {
        return std::nullopt;
    }

    // Firmware counts extruders from 0; every slot number the user reads is
    // 1-based, matching the machine's own labels and the slicer's filament list.
    const int slot = std::stoi(digits) + 1;

    spdlog::warn("{} Resume refused: extruder {} has no filament type assigned", backend_log_tag(),
                 digits);

    // No gcode fixes this in one tap: SET_PRINT_FILAMENT_CONFIG needs a vendor,
    // type and subtype the user has to choose. So the action set is a plain
    // dismiss and the sentence carries the fix. Offering Resume here would
    // rebuild the loop this classifier exists to break.
    std::vector<helix::RecoveryAction> actions;
    actions.push_back(
        {lv_tr("OK"), "", "ams_backend_snapmaker::not_edit_filament_dismiss", "", false});

    helix::ErrorEvent e = helix::make_ams_fault_event(
        helix::ErrorSource::SNAPMAKER, lv_tr("Filament not set"),
        fmt::format(lv_tr("Slot {} has no filament type set, so the printer will not resume. "
                          "Set its material in the filament panel, then resume the print."),
                    slot),
        std::move(actions));
    // make_ams_fault_event leaves raw_detail empty; the router's cross-source
    // dedup matches on the firmware's untranslated wording, not our sentence.
    e.raw_detail = detail;
    return e;
}

std::vector<helix::printer::DeviceSection> AmsBackendSnapmaker::get_device_sections() const {
    if (print_preferences().empty()) {
        return {}; // nothing reported yet - an empty section is worse than none
    }
    using helix::printer::DeviceSection;
    DeviceSection s;
    s.id = "snapmaker_print_prefs";
    s.label = lv_tr("Print Behaviour");
    s.display_order = 50;
    return {s};
}

std::vector<helix::printer::DeviceAction> AmsBackendSnapmaker::get_device_actions() const {
    using helix::printer::ActionType;
    using helix::printer::DeviceAction;

    // A copy taken under mutex_: the member is written on the WebSocket thread.
    const auto p = print_preferences();
    std::vector<DeviceAction> out;

    auto add_toggle = [&](const char* id, const char* label, bool value) {
        DeviceAction a;
        a.id = id;
        a.section = "snapmaker_print_prefs";
        a.label = lv_tr(label);
        a.type = ActionType::TOGGLE;
        a.current_value = value;
        out.push_back(std::move(a));
    };

    // Only settings the firmware has reported become actions: a toggle whose
    // state is unknown renders off and invites "changing" it to its own value.
    if (p.auto_replenish) {
        add_toggle("snapmaker_auto_replenish", "Auto-replenish filament", *p.auto_replenish);
    }
    if (p.replenish_ignore_color) {
        add_toggle("snapmaker_replenish_ignore_color", "Replenish ignores colour",
                   *p.replenish_ignore_color);
    }
    if (p.filament_entangle_detect) {
        add_toggle("snapmaker_entangle_detect", "Detect filament tangles",
                   *p.filament_entangle_detect);
    }
    if (p.filament_entangle_sen) {
        DeviceAction a;
        a.id = "snapmaker_entangle_sen";
        a.section = "snapmaker_print_prefs";
        a.label = lv_tr("Tangle sensitivity");
        a.type = ActionType::DROPDOWN;
        a.options = {"low", "medium", "high"};
        a.current_value = *p.filament_entangle_sen;
        out.push_back(std::move(a));
    }
    if (p.end_led_turn_off) {
        add_toggle("snapmaker_end_led_off", "Turn LED off when the print ends",
                   *p.end_led_turn_off);
    }
    for (size_t t = 0; t < p.end_unload_filament.size(); ++t) {
        DeviceAction a;
        a.id = "snapmaker_end_unload_t" + std::to_string(t);
        a.section = "snapmaker_print_prefs";
        a.label = std::string(lv_tr("Unload at end")) + " - T" + std::to_string(t);
        a.type = ActionType::TOGGLE;
        a.current_value = p.end_unload_filament[t];
        a.slot_index = static_cast<int>(t);
        out.push_back(std::move(a));
    }
    return out;
}

std::string AmsBackendSnapmaker::build_preference_gcode(const std::string& action_id,
                                                        const std::any& value) const {
    snapmaker::PrintPreferences changes;

    // Values arrive from the UI thread's action callback, so every cast is
    // pointer-form: a value whose type does not match the action reads as
    // absent and refuses like a malformed id, never as a bad_any_cast out
    // of the callback.
    if (action_id == "snapmaker_auto_replenish") {
        if (const auto* v = std::any_cast<bool>(&value)) {
            changes.auto_replenish = *v;
        } else {
            return {};
        }
    } else if (action_id == "snapmaker_replenish_ignore_color") {
        if (const auto* v = std::any_cast<bool>(&value)) {
            changes.replenish_ignore_color = *v;
        } else {
            return {};
        }
    } else if (action_id == "snapmaker_entangle_detect") {
        if (const auto* v = std::any_cast<bool>(&value)) {
            changes.filament_entangle_detect = *v;
        } else {
            return {};
        }
    } else if (action_id == "snapmaker_end_led_off") {
        if (const auto* v = std::any_cast<bool>(&value)) {
            changes.end_led_turn_off = *v;
        } else {
            return {};
        }
    } else if (action_id == "snapmaker_entangle_sen") {
        if (const auto* v = std::any_cast<std::string>(&value)) {
            changes.filament_entangle_sen = *v;
        } else {
            return {};
        }
    } else if (action_id.rfind("snapmaker_end_unload_t", 0) == 0) {
        const std::string suffix = action_id.substr(sizeof("snapmaker_end_unload_t") - 1);
        // Malformed ids stop here: a suffix that is not a parsable index
        // names no tool.
        if (!is_parsable_index(suffix)) {
            return {};
        }
        // END_UNLOAD_FILAMENT takes the whole list, so the untouched tools
        // travel at their current values or they are cleared. The snapshot is
        // taken without holding mutex_ across the send that follows: a frame
        // updating a different tool in that window loses to this write, which
        // is the user-write-wins ordering.
        const size_t tool = static_cast<size_t>(std::stoul(suffix));
        auto list = print_preferences().end_unload_filament;
        // A tool past the reported list is a stale id from before the firmware
        // reported fewer toolheads.
        if (tool >= list.size()) {
            return {};
        }
        const auto* v = std::any_cast<bool>(&value);
        if (v == nullptr) {
            return {};
        }
        list[tool] = *v;
        changes.end_unload_filament = std::move(list);
    } else {
        return {};
    }
    return snapmaker::write_print_preferences_gcode(changes);
}

AmsError AmsBackendSnapmaker::execute_device_action(const std::string& action_id,
                                                    const std::any& value) {
    const std::string gcode = build_preference_gcode(action_id, value);
    if (gcode.empty()) {
        return AmsErrorHelper::not_supported(action_id);
    }
    // The firmware refuses END_UNLOAD_FILAMENT while printing or paused unless
    // FORCE=1, which we do not pass. The refusal arrives as a firmware
    // exception and reaches the user through error classification, not here.
    return execute_gcode(gcode);
}

} // namespace helix

#endif // HELIX_HAS_SNAPMAKER
