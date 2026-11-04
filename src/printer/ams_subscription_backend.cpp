// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_subscription_backend.h"

#include "http_executor.h"
#include "lane_echo.h"
#include "lane_legacy_migration.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "moonraker_error.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "toolhead_homing.h"

#include <chrono>
#include <utility>

#include "hv/json.hpp"

namespace helix {

AmsSubscriptionBackend::AmsSubscriptionBackend(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
    : api_(api), client_(client) {
    // Common defaults -- derived constructors set type-specific fields
    system_info_.version = "unknown";
    system_info_.current_tool = -1;
    system_info_.current_slot = -1;
    system_info_.filament_loaded = false;
    system_info_.action = AmsAction::IDLE;
    system_info_.total_slots = 0;
}

AmsSubscriptionBackend::~AmsSubscriptionBackend() {
    {
        std::lock_guard<std::mutex> lock(debounce_cancel_->mutex);
        debounce_cancel_->cancelled = true;
    }
    debounce_cancel_->cv.notify_all();
    // Release without unsubscribe -- MoonrakerClient may already be destroyed
    subscription_.release();
}

bool AmsSubscriptionBackend::debounce_wait(const std::shared_ptr<DebounceCancel>& cancel,
                                           std::chrono::milliseconds window) {
    std::unique_lock<std::mutex> lock(cancel->mutex);
    return !cancel->cv.wait_for(lock, window, [&cancel] { return cancel->cancelled; });
}

AmsError AmsSubscriptionBackend::start() {
    bool should_emit = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (running_) {
            return AmsErrorHelper::success();
        }

        if (!client_) {
            spdlog::error("{} Cannot start: MoonrakerClient is null", backend_log_tag());
            return AmsErrorHelper::not_connected("MoonrakerClient not provided");
        }

        if (!api_) {
            spdlog::error("{} Cannot start: IMoonrakerAPI is null", backend_log_tag());
            return AmsErrorHelper::not_connected("IMoonrakerAPI not provided");
        }

        // Derived class extra checks (e.g., ToolChanger requires tools discovered)
        auto extra_check = additional_start_checks();
        if (!extra_check.success()) {
            return extra_check;
        }

        helix::SubscriptionId id = client_->register_notify_update(
            [this, token = lifetime_.token()](const nlohmann::json& notification) {
                // L081 Mechanism C: handle_status_update mutates members + emits events.
                // High-volume WS notify path: every status frame goes through queue_update,
                // matching the rest of printer state which is already main-thread-marshaled.
                // First-fire baseline state (initial subscription frame from Klipper) used
                // to need defer_critical to survive the splash→home scoped_freeze(); under
                // the new buffer-not-drop semantics, plain defer is sufficient — buffered
                // callbacks splice back into pending_ when the freeze releases.
                token.defer("AmsSubscriptionBackend::notify_update",
                            [this, notification]() { handle_status_update(notification); });
            });

        if (id == helix::kInvalidSubscriptionId) {
            spdlog::error("{} Failed to register for status updates", backend_log_tag());
            return AmsErrorHelper::not_connected("Failed to subscribe to Moonraker updates");
        }

        subscription_ = SubscriptionGuard(client_, id);
        running_ = true;
        spdlog::info("{} Backend started, subscription ID: {}", backend_log_tag(), id);
        should_emit = true;
    }

    // Emit initial state event OUTSIDE the lock to avoid deadlock
    if (should_emit) {
        emit_event(EVENT_STATE_CHANGED);
    }

    // Derived class post-start work (version detection, config loading, etc.)
    on_started();

    return AmsErrorHelper::success();
}

SlotInfo AmsSubscriptionBackend::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const SlotInfo* slot = slot_info_locked(slot_index);
    return slot ? *slot : SlotInfo{};
}

void AmsSubscriptionBackend::handle_status_update(const nlohmann::json& notification) {
    auto params = notification.find("params");
    if (params == notification.end() || !params->is_array() || params->empty() ||
        !(*params)[0].is_object()) {
        return;
    }
    handle_status((*params)[0]);
}

void AmsSubscriptionBackend::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        on_stopping();
        subscription_.reset();
        running_ = false;
        spdlog::info("{} Backend stopped", backend_log_tag());
    }
    on_stopped();
}

std::uint64_t AmsSubscriptionBackend::own_write_echo_sequence(int slot_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    return own_write_echoes_.staged_sequence(slot_index);
}

void AmsSubscriptionBackend::abandon_own_write_echoes(int slot_index,
                                                      std::uint64_t staged_sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    own_write_echoes_.abandon(slot_index, staged_sequence);
}

void AmsSubscriptionBackend::clear_override_locked(int slot_index, SlotInfo* slot) {
    // An unlink marker is not identity, so the clear keeps it on an otherwise
    // empty record: dropping it would let firmware's stale id relink the lane.
    const auto existing = overrides_.find(slot_index);
    const int unlinked_spool_id =
        existing != overrides_.end() ? existing->second.unlinked_spool_id : 0;
    overrides_.erase(slot_index);
    // The lane's own records go with it: a clear that reached only one store
    // would leave resolve() still reporting the identity just removed.
    helix::ams::reset_lane_to_machine_readings(lane_id(slot_index));
    // The user just disowned the write, so what firmware repeats from here on
    // is its own word again.
    own_write_echoes_.abandon(slot_index);

    if (slot) {
        clear_override_fields(*slot);
    }

    if (unlinked_spool_id > 0) {
        helix::ams::FilamentSlotOverride& marker = overrides_[slot_index];
        marker.unlinked_spool_id = unlinked_spool_id;
        if (override_store_) {
            helix::ams::save_override_async(override_store_.get(), slot_index, marker,
                                            backend_log_tag(), "unlink");
        }
    } else if (override_store_) {
        // Capture by value: clear_async's Moonraker callback can fire after
        // the backend itself is gone.
        const std::string tag = backend_log_tag();
        override_store_->clear_async(slot_index, [tag, slot_index](bool ok, std::string err) {
            if (!ok) {
                spdlog::warn("{} clear_async failed for slot {}: {}", tag, slot_index, err);
            }
        });
    }
}

void AmsSubscriptionBackend::clear_override_fields(SlotInfo& slot) const {
    slot.brand.clear();
    slot.clear_spoolman_link();
    slot.remaining_weight_g = -1.0f;
    slot.total_weight_g = -1.0f;
    slot.color_name.clear();
    slot.catalog_id.clear();
    slot.product_name.clear();
}

void AmsSubscriptionBackend::release_subscriptions() {
    subscription_.release();
}

bool AmsSubscriptionBackend::is_running() const {
    return running_;
}

void AmsSubscriptionBackend::request_resync() {
    // A lane whose firmware states its own identity already has a producer on
    // the vendor-cache slot, and a second one there races it. With nothing to
    // file, the round-trip has no reader, so it is not made at all.
    if (firmware_publishes_lane_identity()) {
        return;
    }
    auto* store = lane_record_store();
    if (!store) {
        // The persisted record is this backend's identity producer (firmware
        // states none of its own) and there is no store to read it from, so
        // the resync cannot be issued. Only a started backend with an
        // attached API owes a store: a store is built in on_started(), and
        // one with no API never builds it, so a pre-start or API-less resync
        // is expected to find nothing and stays quiet.
        if (running_ && api_) {
            spdlog::warn("{} request_resync(): no lane-record store to re-read; resync skipped",
                         backend_log_tag());
        }
        return;
    }
    // The record map is keyed by slot index, so the lambda needs this
    // backend's block to name a lane. Carried by value: the deferred callback
    // has no claim on `this` by the time it runs, and backend_index() is
    // stamped by registration, which every resync call site is downstream of.
    // `self` rides the reload callback un-dereferenced; it is reached only
    // inside the token-guarded defer, which skips a dead owner.
    const int block = backend_index();
    auto token = lifetime_.token();
    AmsSubscriptionBackend* self = this;
    store->reload_async([token, block,
                         self](std::unordered_map<int, helix::ams::LaneDataRecord> records) {
        token.defer("AmsSubscriptionBackend::resync_lane_records",
                    [self, block, records = std::move(records)]() {
                        for (const auto& [slot, entry] : records) {
                            // Only what the namespace merely remembers is
                            // re-filed. A record naming a spool is the
                            // server's statement and one declaring a colour or material
                            // is a person's; re-filing either would forge a
                            // declaration out of a re-read, which is the
                            // confusion the source model exists to end.
                            //
                            // Remembered rather than VendorCache because this
                            // re-reads our own store, not a firmware frame.
                            helix::ams::Observation obs =
                                helix::ams::declared_from_record(entry.record);
                            if (obs.source != helix::ams::ObservationSource::Remembered) {
                                continue;
                            }
                            // A record another tool wrote is the newest
                            // statement on the lane when it displaces what
                            // stands there (prestonbrown/helixscreen#1632):
                            // it files on the user's rung, where it
                            // resolves over firmware's cache and yields to
                            // whatever edit lands next. A record the
                            // backend's own firmware plugin wrote is a
                            // reading, not an edit, and keeps filing as
                            // remembered; anything else does too, below
                            // whatever a person said.
                            const helix::ams::LaneId lane = helix::ams::lane_id_for(block, slot);
                            if (self->file_reread_outside_edit(lane, slot, entry, obs)) {
                                continue;
                            }
                            helix::ams::ingest(lane, obs);
                        }
                    });
    });
}

bool AmsSubscriptionBackend::file_reread_outside_edit(helix::ams::LaneId lane, int slot_index,
                                                      const helix::ams::LaneDataRecord& entry,
                                                      helix::ams::Observation& obs) {
    bool write_in_flight = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        own_write_echoes_.strip_standing(slot_index, obs);
        write_in_flight = own_write_echoes_.standing(slot_index);
    }
    return helix::ams::file_outside_edit_if_newer(lane, entry, obs, write_in_flight);
}

void AmsSubscriptionBackend::schedule_lane_record_reread() {
    on_lane_reread_scheduled();
    if (lane_reread_pending_.exchange(true)) {
        return;
    }
    auto token = lifetime_.token();
    helix::http::HttpExecutor::fast().submit([this, token, cancel = debounce_cancel_]() {
        // BG THREAD: only the debounce sleep.
        if (!debounce_wait(cancel, std::chrono::seconds(2))) {
            return;
        }
        token.defer("AmsSubscriptionBackend::lane_record_reread", [this]() {
            lane_reread_pending_.store(false);
            reread_lane_records();
        });
    });
}

void AmsSubscriptionBackend::seed_lane_records_seen_locked() {
    lane_wire_seen_.clear();
    if (!override_store_) {
        return;
    }
    for (const auto& [slot, entry] : override_store_->last_lane_data_records()) {
        lane_wire_seen_[slot] = entry.wire;
    }
}

void AmsSubscriptionBackend::reread_lane_records() {
    helix::ams::FilamentSlotOverrideStore* store = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        store = override_store_.get();
    }
    if (!store) {
        on_lane_reread_finished({});
        return;
    }
    auto token = lifetime_.token();
    // A read that never lands took no slot.
    store->reload_async(
        [this, token](std::unordered_map<int, helix::ams::LaneDataRecord> records) {
            token.defer(
                "AmsSubscriptionBackend::apply_lane_records",
                [this, records = std::move(records)]() { apply_reread_lane_records(records); });
        },
        [this, token]() {
            token.defer("AmsSubscriptionBackend::lane_reread_failed", [this]() {
                spdlog::debug("{} lane_data re-read failed; the next change retries",
                              backend_log_tag());
                on_lane_reread_finished({});
            });
        });
}

void AmsSubscriptionBackend::apply_reread_lane_records(
    const std::unordered_map<int, helix::ams::LaneDataRecord>& records) {
    std::unordered_set<int> taken;
    for (const auto& [slot, entry] : records) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (slot < 0 || slot >= slot_index_bound_locked()) {
                continue;
            }
            auto seen = lane_wire_seen_.find(slot);
            const bool unchanged = seen != lane_wire_seen_.end() && seen->second == entry.wire;
            lane_wire_seen_[slot] = entry.wire;
            if (unchanged) {
                continue;
            }
        }
        // Every field the record states, not only what request_resync() would
        // re-file as remembered: the spool link is what this read is for.
        helix::ams::Observation obs = helix::ams::declared_from_record(entry.record);
        const auto lane = lane_id(slot);
        const bool rebinds = helix::ams::outside_record_rebinds(lane, obs);
        if (!file_reread_outside_edit(lane, slot, entry, obs)) {
            continue;
        }
        spdlog::info("{} Slot {} lane_data rewritten by another tool; taking its record",
                     backend_log_tag(), slot);
        taken.insert(slot);
        {
            // The override follows the lane: a rebinding starts it afresh, and
            // anything else amends it, so a later save neither republishes
            // what the tool replaced nor drops what it left alone.
            std::lock_guard<std::mutex> lock(mutex_);
            auto& ovr = overrides_[slot];
            if (rebinds) {
                helix::ams::FilamentSlotOverride fresh;
                fresh.fingerprint = ovr.fingerprint;
                fresh.unlinked_spool_id = ovr.unlinked_spool_id;
                ovr = std::move(fresh);
            }
            helix::ams::amend_record(ovr, obs);
        }
        repaint_slot_from_lane(slot);
        emit_event(kEventSlotChanged, std::to_string(slot));
    }
    on_lane_reread_finished(taken);
}

void AmsSubscriptionBackend::repaint_slot_from_lane(int slot_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (SlotInfo* slot = cached_slot_locked(slot_index)) {
        prepare_lane_repaint_locked(slot_index, *slot);
        apply_resolved_lane(*slot, slot_index);
    }
}

void AmsSubscriptionBackend::set_event_callback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_callback_ = std::move(callback);
}

void AmsSubscriptionBackend::emit_event(const std::string& event, const std::string& data) {
    EventCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = event_callback_;
    }
    if (cb) {
        cb(event, data);
    }
}

void AmsSubscriptionBackend::on_klipper_restarted() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ams_action_is_busy(system_info_.action)) {
            return;
        }
        spdlog::warn("{} {} ended with a Klipper restart; no longer reporting it busy",
                     backend_log_tag(), ams_action_to_string(system_info_.action));
        system_info_.action = AmsAction::IDLE;
        system_info_.operation_detail.clear();
    }
    emit_event(EVENT_STATE_CHANGED);
}

AmsAction AmsSubscriptionBackend::get_current_action() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_action_locked();
}

int AmsSubscriptionBackend::get_current_tool() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.current_tool;
}

int AmsSubscriptionBackend::get_current_slot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.current_slot;
}

bool AmsSubscriptionBackend::is_filament_loaded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.filament_loaded;
}

std::vector<int> AmsSubscriptionBackend::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.tool_to_slot_map;
}

AmsError AmsSubscriptionBackend::set_tool_mapping_impl(int tool_number, int slot_index) {
    return can_set_tool_mapping(tool_number, slot_index);
}

bool AmsSubscriptionBackend::op_moves_toolhead(FilamentOp op) const {
    switch (op) {
    case FilamentOp::Load:
    case FilamentOp::Unload:
    case FilamentOp::ChangeTool:
    case FilamentOp::ParkTool:
        // Pushing or pulling filament through the hotend, and swapping what is
        // on the carriage, are toolhead motion on every backend there is. No
        // override path exists for these on purpose.
        return true;
    case FilamentOp::SelectSlot:
        // The one genuinely per-backend answer. See select_slot_moves_toolhead().
        return select_slot_moves_toolhead();
    }
    return true; // Unreachable; fail closed if the enum ever grows.
}

AmsError AmsSubscriptionBackend::claim_filament_op(FilamentOp op, bool check_state) {
    AmsAction pending = AmsAction::LOADING;
    switch (op) {
    case FilamentOp::Load:
        pending = AmsAction::LOADING;
        break;
    case FilamentOp::Unload:
    case FilamentOp::ParkTool:
        pending = AmsAction::UNLOADING;
        break;
    case FilamentOp::SelectSlot:
    case FilamentOp::ChangeTool:
        pending = AmsAction::SELECTING;
        break;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // The started/busy read and the claim share ONE critical section. Split
    // across two, a second op could read IDLE between the first op's read and
    // its claim.
    //
    // The read is under mutex_ for the same reason every other system_info_ read
    // in this class is: it is the field's declared discipline. Every writer today
    // happens to land on the main thread — handle_status_update() and the gcode
    // acks all marshal through token.defer() — so an unlocked read here was
    // formally a race and practically quiet. Do not take that as licence to skip
    // the lock; the next background writer would make it loud.
    if (check_state) {
        if (auto e = state_preconditions_unlocked(); !e.success()) {
            return e;
        }
    }
    if (filament_op_in_flight_) {
        return AmsErrorHelper::busy(ams_action_to_string(filament_op_claimed_action_));
    }
    filament_op_in_flight_ = true;
    filament_op_claimed_action_ = pending;
    return AmsErrorHelper::success();
}

void AmsSubscriptionBackend::release_filament_op_claim() {
    std::lock_guard<std::mutex> lock(mutex_);
    filament_op_in_flight_ = false;
    filament_op_claimed_action_ = AmsAction::IDLE;
}

AmsError AmsSubscriptionBackend::run_filament_op(FilamentOp op, int arg,
                                                 const std::vector<int>* batch) {
    // Order of refusals is load-bearing and matches what check_preconditions()
    // has always produced: not-started, then busy, then print-active.
    if (auto e = claim_filament_op(op, filament_op_gate() == FilamentOpGate::Standard);
        !e.success()) {
        return e;
    }
    // Owns the claim from here. Every return below releases it, including the
    // print refusal — a claim that outlived a refused op would wedge the backend
    // into a permanent busy with no action to explain it.
    FilamentOpClaim claim(this);

    // Deliberately NOT under mutex_: this reads PrinterState, which has its own
    // synchronization and nothing in system_info_ to be atomic with, and the
    // claim already excludes a second op for the whole window.
    if (op_moves_toolhead(op)) {
        // Parking leaves no tool for a paused job to resume with, so a paused
        // job refuses it on every backend, whatever the others allow.
        if (auto e = refuse_if_printing(/*paused_blocks=*/op == FilamentOp::ParkTool);
            !e.success()) {
            return e;
        }
    }

    // mutex_ is NOT held across the hook. The hooks issue gcode and Moonraker
    // JSON-RPC, several call emit_event() (which takes mutex_ to copy the
    // callback), and AD5X's unload re-enters eject_lane() which locks — holding
    // it here would deadlock on the first two and serialize the network on the
    // third. The claim is a flag, not a lock: a contending op is refused
    // immediately rather than blocked behind the send.
    if (batch != nullptr) {
        return do_filament_batch(*batch, op == FilamentOp::Load);
    }
    switch (op) {
    case FilamentOp::Load:
        return do_load_filament(arg);
    case FilamentOp::Unload:
        return do_unload_filament(arg);
    case FilamentOp::SelectSlot:
        return do_select_slot(arg);
    case FilamentOp::ChangeTool:
        return do_change_tool(arg);
    case FilamentOp::ParkTool:
        return do_park_active_tool();
    }
    return AmsErrorHelper::success(); // Unreachable; the enum is exhaustive.
}

AmsError AmsSubscriptionBackend::load_filament(int slot_index) {
    return run_filament_op(FilamentOp::Load, slot_index);
}

AmsError AmsSubscriptionBackend::unload_filament(int slot_index) {
    return run_filament_op(FilamentOp::Unload, slot_index);
}

AmsError AmsSubscriptionBackend::select_slot(int slot_index) {
    return run_filament_op(FilamentOp::SelectSlot, slot_index);
}

AmsError AmsSubscriptionBackend::change_tool(int tool_number) {
    return run_filament_op(FilamentOp::ChangeTool, tool_number);
}

AmsError AmsSubscriptionBackend::park_active_tool() {
    // Unsupported is the answer whatever state the backend is in, so it comes
    // before the gate's not-started and busy refusals.
    if (!supports_tool_park()) {
        return AmsErrorHelper::not_supported("Tool park");
    }
    return run_filament_op(FilamentOp::ParkTool, -1);
}

AmsError AmsSubscriptionBackend::load_filament_batch(const std::vector<int>& slots) {
    return run_filament_op(FilamentOp::Load, -1, &slots);
}

AmsError AmsSubscriptionBackend::unload_filament_batch(const std::vector<int>& slots) {
    return run_filament_op(FilamentOp::Unload, -1, &slots);
}

AmsError AmsSubscriptionBackend::state_preconditions_unlocked() const {
    if (!running_) {
        return AmsErrorHelper::not_connected(std::string(backend_log_tag()) +
                                             " backend not started");
    }
    if (system_info_.is_busy()) {
        return AmsErrorHelper::busy(ams_action_to_string(system_info_.action));
    }
    // A calibration or tool cycle the printer is still running: Klipper would
    // queue the op behind it, to run unattended minutes later.
    if (tracked_run_active_.load()) {
        return AmsErrorHelper::busy("a device action");
    }
    return AmsErrorHelper::success();
}

AmsError AmsSubscriptionBackend::validate_slot_index_locked(int slot_index) const {
    const int bound = slot_index_bound_locked();
    if (bound <= 0) {
        return AmsErrorHelper::no_slots_discovered(lane_noun());
    }
    if (slot_index < 0 || slot_index >= bound || system_info_.slot_absent(slot_index)) {
        return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, bound - 1);
    }
    return AmsErrorHelper::success();
}

AmsError AmsSubscriptionBackend::validate_slot_index(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return validate_slot_index_locked(slot_index);
}

AmsError AmsSubscriptionBackend::check_preconditions(bool requires_toolhead_motion) const {
    if (auto e = state_preconditions_unlocked(); !e.success()) {
        return e;
    }
    // Toolhead-motion ops (load/unload/tool-change) additionally refuse while a
    // print is active — no-motion ops (eject_lane, select, unlock) pass false.
    if (requires_toolhead_motion) {
        if (auto e = refuse_if_printing(); !e.success()) {
            return e;
        }
    }
    return AmsErrorHelper::success();
}

AmsError AmsSubscriptionBackend::refuse_if_printing(bool paused_blocks) const {
    // The AmsError face of print_blocks_filament_op(), which states the rule and
    // why each lifecycle answers as it does. Only the parts specific to being
    // the BACKEND end of it live here.
    //
    // This layer has to cover the window Layer 1 cannot. Layer 1
    // (helix::api::reject_homing_during_active_print) refuses every app-emitted
    // G28 while PRINTING or PAUSED, but it cannot see a `_G28` buried inside a
    // firmware macro, which is what filament_ops_self_home() reports and why a
    // self-homing backend is refused even while paused (bundle XWPBR2DX). Layer 1
    // also deliberately stops short of PREPARING — it would refuse the app's own
    // pre-start block, sent through execute_gcode() and free to contain a G28 —
    // so PREPARING has to be caught here or nowhere.
    //
    // api_ can be null in unit tests / cold-boot; when it is, print state is
    // unknown and we do not block (mirrors ensure_homed_then's null-client path).
    if (!api_) {
        return AmsErrorHelper::success();
    }
    const auto lifecycle = api_->printer_state().print_state().get_print_lifecycle();
    const bool is_paused = (lifecycle == PrintState::Paused);
    const bool self_homes = paused_blocks || filament_ops_self_home();
    // print_blocks_filament_op() is the rule, shared with every surface that
    // greys a filament-op button. This function's job is to map its answer onto
    // an AmsError, not to decide it a second time.
    if (!print_blocks_filament_op(lifecycle, self_homes)) {
        if (is_paused) {
            spdlog::info("{} Allowing filament operation on a PAUSED print "
                         "(backend does not self-home; Layer 1 still blocks any G28)",
                         backend_log_tag());
        }
        return AmsErrorHelper::success();
    }
    spdlog::warn("{} Refusing filament operation while a print is active (lifecycle={}, "
                 "self_homes={})",
                 backend_log_tag(), static_cast<int>(lifecycle), self_homes);
    // A non-self-homing backend that reaches here is PRINTING, and pausing is a
    // recovery it can actually offer — say so instead of "finish or cancel".
    return AmsErrorHelper::print_active(is_paused, /*pause_allows_ops=*/!self_homes);
}

bool AmsSubscriptionBackend::print_holds_printer() const {
    // self_homes=true: PAUSED holds it too, the same span machine_motion_blocked greys.
    return api_ &&
           print_blocks_filament_op(api_->printer_state().print_state().get_print_lifecycle(),
                                    /*backend_self_homes=*/true);
}

AmsError AmsSubscriptionBackend::refuse_motion_if_printing() const {
    if (!print_holds_printer()) {
        return AmsErrorHelper::success();
    }
    spdlog::warn("{} Refusing machine motion while a print holds the printer (lifecycle={})",
                 backend_log_tag(),
                 static_cast<int>(api_->printer_state().print_state().get_print_lifecycle()));
    return AmsErrorHelper::busy_with_print();
}

AmsError AmsSubscriptionBackend::send_endless_spool_switch(const std::string& gcode,
                                                           std::function<void()> on_sent) {
    auto token = lifetime_.token();
    return execute_gcode(
        gcode,
        [on_sent = std::move(on_sent)]() {
            if (on_sent) {
                on_sent();
            }
        },
        [this, token](const MoonrakerError&) {
            token.defer("AmsSubscriptionBackend::endless_spool_refused",
                        [this]() { emit_event(kEventEndlessSpoolRefused); });
        });
}

AmsError AmsSubscriptionBackend::device_action_refusal(const std::string& action_id) const {
    // Only an action the backend offers right now runs: one it no longer lists
    // (its hardware went away, its config changed) has no moves_machine answer
    // to gate it by.
    const auto actions = get_device_actions();
    const auto it = std::find_if(actions.begin(), actions.end(),
                                 [&](const auto& a) { return a.id == action_id; });
    if (it == actions.end()) {
        spdlog::warn("{} Refusing device action '{}': not offered now", backend_log_tag(),
                     action_id);
        return AmsErrorHelper::not_supported("Device action " + action_id);
    }
    // Listed but unavailable (no hardware for it, config still loading): every
    // caller is refused, the UI's own checks aside.
    if (!it->enabled) {
        spdlog::warn("{} Refusing device action '{}': {}", backend_log_tag(), action_id,
                     it->disable_reason);
        return AmsError(AmsResult::WRONG_STATE, "Device action " + action_id + " is unavailable",
                        lv_tr("Not available now"), lv_tr(it->disable_reason.c_str()));
    }
    if (it->moves_machine) {
        if (tracked_run_active_.load()) {
            return AmsErrorHelper::busy("a device action");
        }
        return it->allowed_while_paused ? refuse_if_printing() : refuse_motion_if_printing();
    }
    return AmsErrorHelper::success();
}

bool AmsSubscriptionBackend::begin_tracked_run() {
    bool expected = false;
    return tracked_run_active_.compare_exchange_strong(expected, true);
}

void AmsSubscriptionBackend::end_tracked_run() {
    tracked_run_active_.store(false);
}

AmsError AmsSubscriptionBackend::execute_device_action(const std::string& action_id,
                                                       const std::any& value) {
    if (auto refused = device_action_refusal(action_id); !refused.success()) {
        return refused;
    }
    return do_execute_device_action(action_id, value);
}

AmsError AmsSubscriptionBackend::run_device_action(const std::string& action_id,
                                                   const std::any& value,
                                                   DeviceActionDone on_done) {
    if (auto refused = device_action_refusal(action_id); !refused.success()) {
        return refused;
    }
    return do_run_device_action(action_id, value, std::move(on_done));
}

AmsError AmsSubscriptionBackend::do_run_device_action(const std::string& action_id,
                                                      const std::any& value,
                                                      DeviceActionDone on_done) {
    AmsError result = do_execute_device_action(action_id, value);
    if (result.success() && on_done) {
        on_done(result);
    }
    return result;
}

AmsError AmsSubscriptionBackend::run_edit_follow_up(int slot_index, const SlotInfo& before,
                                                    const SlotInfo& after) {
    if (auto refused = refuse_if_printing(/*paused_blocks=*/true); !refused.success()) {
        return refused;
    }
    return do_run_edit_follow_up(slot_index, before, after);
}

bool AmsSubscriptionBackend::toolhead_homed() const {
    if (!api_) {
        // No connection: callers fall back to dispatching directly, so the
        // answer is not consulted. Report homed so no G28 is ever synthesized
        // against a printer we cannot talk to.
        return true;
    }
    return helix::toolhead_is_homed(api_->printer_state());
}

AmsError AmsSubscriptionBackend::ensure_homed_then(
    std::string gcode, helix::SharedCallback<void()> on_complete,
    helix::SharedCallback<void(const MoonrakerError&)> on_error, uint32_t timeout_ms,
    bool skip_homing, bool silent, std::optional<bool> caller_surfaces_errors,
    helix::SharedCallback<void(const MoonrakerError&)> on_predispatch_error,
    bool bypass_busy_gate) {
    // The homed answer comes from the live homed_axes subject, not an RPC:
    // toolhead is in the standing objects.subscribe set, so querying it again
    // was a redundant round trip. skip_homing short-circuits the check
    // entirely -- toolhead_homed() is never called -- for firmware macros that
    // home themselves (CFS Fork variant). delegates_homing_to_printer()
    // short-circuits the same way when the printer-side system homes (AFC
    // auto_home) — no G28 from us.
    if (skip_homing || delegates_homing_to_printer() || toolhead_homed()) {
        return dispatch_payload(std::move(gcode), std::move(on_complete), std::move(on_error),
                                timeout_ms, silent, caller_surfaces_errors, bypass_busy_gate);
    }

    // An operation that needs a home just homes first, with no confirmation:
    // asking for the op is consent to the home it requires.
    spdlog::info("{} Sending G28 before operation", backend_log_tag());
    // A failure here happens BEFORE the payload ships, so it routes to
    // on_predispatch_error when the caller supplied one.
    auto pre_err = on_predispatch_error ? on_predispatch_error : on_error;

    // No API: emit the G28 through the VIRTUAL execute_gcode rather than
    // skipping it. api_ is null only in fixtures, and they override the
    // virtual to capture - routing around it here would make the unhomed
    // branch untestable and silently drop the G28 from the recorded sequence.
    // The real path below cannot use the virtual because it needs an error
    // callback and kHomingTimeoutMs, which the virtual forms do not take.
    // Fixtures are synchronous (no real RPC), so the AmsError the virtual
    // returns IS the only failure signal available here -- there is no async
    // MoonrakerError to forward, so a failure is translated into one.
    if (!api_) {
        AmsError g28_result = execute_gcode("G28");
        if (!g28_result.success()) {
            MoonrakerError synthetic;
            synthetic.type = MoonrakerErrorType::UNKNOWN;
            synthetic.message = g28_result.technical_msg;
            handle_dispatch_error(synthetic, pre_err);
            return AmsErrorHelper::success();
        }
        dispatch_payload(std::move(gcode), std::move(on_complete), std::move(on_error), timeout_ms,
                         silent, caller_surfaces_errors, bypass_busy_gate);
        return AmsErrorHelper::success();
    }

    auto token = lifetime_.token();
    api_->execute_gcode(
        "G28",
        [this, token, gcode = std::move(gcode), on_complete, on_error, timeout_ms, silent,
         caller_surfaces_errors, bypass_busy_gate]() {
            // L081 Mechanism C: the ack lands on a bg thread and dispatch_payload
            // touches api_/members. Marshal to main.
            token.defer("AmsSubscriptionBackend::ensure_homed_then_g28_success",
                        [this, gcode, on_complete, on_error, timeout_ms, silent,
                         caller_surfaces_errors, bypass_busy_gate]() {
                            dispatch_payload(gcode, on_complete, on_error, timeout_ms, silent,
                                             caller_surfaces_errors, bypass_busy_gate);
                        });
        },
        [this, token, pre_err](const MoonrakerError& err) {
            token.defer("AmsSubscriptionBackend::ensure_homed_then_g28_error",
                        [this, err, pre_err]() { handle_dispatch_error(err, pre_err); });
        },
        IMoonrakerAPI::kHomingTimeoutMs, /*silent=*/false, /*on_queued=*/nullptr,
        // The G28 error leg lands in handle_dispatch_error(), which without a
        // caller callback only logs and resets the action to IDLE. Its ownership
        // answer is the caller's, not this wrapper's — and on this leg the
        // caller's callback is pre_err.
        caller_surfaces_errors.value_or(pre_err != nullptr));

    return AmsErrorHelper::success();
}

void AmsSubscriptionBackend::handle_dispatch_error(
    const MoonrakerError& err, const helix::SharedCallback<void(const MoonrakerError&)>& on_error) {
    if (on_error) {
        on_error(err);
        return;
    }
    // Historical behaviour, preserved exactly for the 8 pre-existing callers
    // (none of which pass on_error): log and reset to IDLE so the UI doesn't
    // get stuck on a "loading" spinner after a failed G28/payload.
    spdlog::error("{} G-code failed: {}", backend_log_tag(), err.message);
    std::lock_guard<std::mutex> lock(mutex_);
    system_info_.action = AmsAction::IDLE;
}

AmsError AmsSubscriptionBackend::refuse_dispatch_no_api(
    const helix::SharedCallback<void(const MoonrakerError&)>& on_error) {
    MoonrakerError synthetic;
    synthetic.type = MoonrakerErrorType::CONNECTION_LOST;
    synthetic.message = "IMoonrakerAPI not available";
    handle_dispatch_error(synthetic, on_error);
    return AmsErrorHelper::not_connected("IMoonrakerAPI not available");
}

AmsError AmsSubscriptionBackend::dispatch_payload(
    std::string gcode, helix::SharedCallback<void()> on_complete,
    helix::SharedCallback<void(const MoonrakerError&)> on_error, uint32_t timeout_ms, bool silent,
    std::optional<bool> caller_surfaces_errors, bool bypass_busy_gate) {
    // Default shape: dispatch through the 1-arg and 2-arg execute_gcode()
    // virtuals. ~20 fixtures override ONLY those two (see the
    // dispatch_payload() doc comment in the header); a payload with no
    // on_complete must reach the 1-arg override or those tests silently stop
    // capturing anything. Callers that pass no on_error/timeout_ms/silent
    // always land here.
    if (!on_error && timeout_ms == IMoonrakerAPI::kAmsOperationTimeoutMs && silent) {
        return on_complete ? execute_gcode(gcode, std::move(on_complete)) : execute_gcode(gcode);
    }

    const char* tag = backend_log_tag();
    auto token = lifetime_.token();

    // An error callback that only unwinds state, at the standard timeout, is
    // exactly the 4-arg execute_gcode() virtual's contract, so it goes through
    // that virtual: fixtures capture it there the way they capture the shapes
    // above. That virtual already runs the completion on main; the error leg
    // still hops here because a refusal for want of an IMoonrakerAPI arrives
    // synchronously, and handle_dispatch_error() takes mutex_.
    // A busy-gate bypass with a live API needs the direct path below: the
    // virtual carries no such flag. Fixtures (no API) still capture through it.
    if (timeout_ms == IMoonrakerAPI::kAmsOperationTimeoutMs &&
        !caller_surfaces_errors.value_or(on_error != nullptr) && !(bypass_busy_gate && api_)) {
        return execute_gcode(
            gcode, std::move(on_complete),
            [this, token, on_error](const MoonrakerError& err) {
                token.defer("AmsSubscriptionBackend::dispatch_payload_error",
                            [this, err, on_error]() { handle_dispatch_error(err, on_error); });
            },
            silent);
    }

    // A caller asked for its own error/timeout/toast policy -- the hardcoded
    // virtuals above can't carry that (they fix kAmsOperationTimeoutMs and
    // silent=true, and their error handling only logs). Talk to IMoonrakerAPI
    // directly.
    if (!api_) {
        return refuse_dispatch_no_api(on_error);
    }

    // Captured from the CALLER's on_error, before the wrapper below makes the
    // callback we hand to IMoonrakerAPI unconditionally non-null. A caller that
    // passed none gets handle_dispatch_error()'s log-and-reset-to-IDLE, which no
    // user ever sees, so the `!!` router must keep the report. A caller whose
    // own on_error also only logs overrides this explicitly (CFS does). See
    // include/rpc_error_policy.h.
    const bool caller_surfaces = caller_surfaces_errors.value_or(on_error != nullptr);

    api_->execute_gcode(
        gcode,
        [tag, token, on_complete = std::move(on_complete)]() {
            spdlog::debug("{} G-code executed successfully", tag);
            if (!on_complete) {
                return;
            }
            // The answer lands on the libhv response thread, while the status
            // frames that arrived before it are still queued for main. A
            // completion that judges state (CFS reads the toolhead switch) must
            // run behind them, on main, or it reads values those frames are
            // about to replace.
            token.defer("AmsSubscriptionBackend::dispatch_payload_complete",
                        [on_complete]() { on_complete(); });
        },
        [this, token, on_error](const MoonrakerError& err) {
            // L081 Mechanism C: this lands on the libhv response thread and
            // touches system_info_/mutex_ (default path) or arbitrary caller
            // logic (on_error). Marshal to main either way.
            token.defer("AmsSubscriptionBackend::dispatch_payload_error",
                        [this, err, on_error]() { handle_dispatch_error(err, on_error); });
        },
        timeout_ms, silent, /*on_queued=*/nullptr, caller_surfaces, bypass_busy_gate);
    return AmsErrorHelper::success();
}

AmsError AmsSubscriptionBackend::execute_gcode(const std::string& gcode) {
    if (!api_) {
        return AmsErrorHelper::not_connected("IMoonrakerAPI not available");
    }
    const char* tag = backend_log_tag();
    spdlog::info("{} Executing G-code: {}", tag, gcode);
    api_->execute_gcode(
        gcode, [tag]() { spdlog::debug("{} G-code executed successfully", tag); },
        [tag, gcode](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("{} G-code response timed out (may still be running): {}", tag, gcode);
            } else if (err.type == MoonrakerErrorType::NOT_READY) {
                // IMoonrakerAPI already logs a [warning] when refusing g-code on a halted
                // Klipper. AD5X-IFS retries _IFS_VARS on every Adventurer5M.json poll, so
                // duplicating at [error] floods the log post-halt.
                spdlog::debug("{} G-code skipped (Klipper halted): {}", tag, gcode);
            } else {
                spdlog::error("{} G-code failed: {} - {}", tag, gcode, err.message);
            }
        },
        IMoonrakerAPI::kAmsOperationTimeoutMs,
        // silent=true: the phase tracker + IFS_STATUS own operation completion,
        // so the RPC timeout is advisory for these long macros. A cold-start
        // load can legitimately run past the 300s ceiling (bundle 77TDH9N6:
        // heat-from-cold + load + double purge + clean), completing fine ~47s
        // later — silent suppresses ONLY the false REQUEST_TIMEOUT toast and the
        // tracker's generic fallback.
        /*silent=*/true, /*on_queued=*/nullptr,
        // caller_surfaces_errors=false: the error_cb above only writes to the
        // log, which the user never sees. Declaring otherwise would silence
        // GcodeErrorRouter, so a real AFC/Happy Hare/CFS macro rejection would
        // reach nobody. With this false, Klipper's `!!` broadcast stays free to
        // surface it. See include/rpc_error_policy.h.
        /*caller_surfaces_errors=*/false);
    return AmsErrorHelper::success();
}

AmsError AmsSubscriptionBackend::execute_gcode(const std::string& gcode,
                                               helix::SharedCallback<void()> on_complete) {
    // silent=true: completion is owned by the phase tracker + IFS_STATUS, not
    // the RPC return, so the advisory 300s timeout must not surface a false
    // "timed out" toast for a legitimately-long macro (bundle 77TDH9N6),
    // along with the tracker's generic fallback.
    return AmsSubscriptionBackend::execute_gcode(gcode, std::move(on_complete), nullptr,
                                                 /*silent=*/true);
}

AmsError AmsSubscriptionBackend::execute_gcode(
    const std::string& gcode, helix::SharedCallback<void()> on_complete,
    helix::SharedCallback<void(const MoonrakerError&)> on_error, bool silent) {
    if (!api_) {
        // The send never went out, but on_error still owes the caller its
        // unwind: an optimistic AmsAction set before this dispatch stays set
        // forever if only the return value reports the refusal. Callers may
        // hold mutex_ here, so this leg touches no backend state itself.
        if (on_error) {
            MoonrakerError synthetic;
            synthetic.type = MoonrakerErrorType::CONNECTION_LOST;
            synthetic.message = "IMoonrakerAPI not available";
            on_error(synthetic);
        }
        return AmsErrorHelper::not_connected("IMoonrakerAPI not available");
    }
    const char* tag = backend_log_tag();
    spdlog::info("{} Executing G-code: {}", tag, gcode);
    auto token = lifetime_.token();
    api_->execute_gcode(
        gcode,
        [tag, token, on_complete = std::move(on_complete)]() {
            spdlog::debug("{} G-code executed successfully", tag);
            if (!on_complete) {
                return;
            }
            // The answer lands on the libhv response thread, while the status
            // frames that arrived before it are still queued for main. The
            // completion runs behind them, on main, so it judges the state
            // those frames report rather than the state they are about to
            // replace.
            token.defer("AmsSubscriptionBackend::execute_gcode_complete",
                        [on_complete]() { on_complete(); });
        },
        [tag, token, gcode, on_error = std::move(on_error)](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("{} G-code response timed out (may still be running): {}", tag, gcode);
            } else if (err.type == MoonrakerErrorType::NOT_READY) {
                spdlog::debug("{} G-code skipped (Klipper halted): {}", tag, gcode);
            } else {
                spdlog::error("{} G-code failed: {} - {}", tag, gcode, err.message);
            }
            if (!on_error) {
                return;
            }
            // Same thread and ordering as the completion. A refusal can also
            // arrive synchronously, on the caller's stack, while it holds
            // mutex_.
            token.defer("AmsSubscriptionBackend::execute_gcode_error",
                        [on_error, err]() { on_error(err); });
        },
        IMoonrakerAPI::kAmsOperationTimeoutMs, silent, /*on_queued=*/nullptr,
        // caller_surfaces_errors=false: on_error unwinds caller state but does
        // not put anything on screen, so declaring otherwise would silence
        // GcodeErrorRouter and a real macro rejection would reach nobody.
        // See include/rpc_error_policy.h.
        /*caller_surfaces_errors=*/false);
    return AmsErrorHelper::success();
}

} // namespace helix
