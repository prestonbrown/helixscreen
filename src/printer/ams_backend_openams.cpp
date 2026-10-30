// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_openams.h"

#include "ui_insert_notice.h"
#include "ui_update_queue.h"

#include "ams_fault_event.h"
#include "i_moonraker_api.h"
#include "lane_apply.h"
#include "lane_legacy_migration.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "lvgl.h"
#include "openams_api.h"
#include "openams_status_parse.h"
#include "printer_discovery.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

#include "hv/json.hpp"

namespace helix {
namespace {

using json = nlohmann::json;

constexpr const char* kSpoolmanStatusMethod = "notify_openams_spoolman_status";
constexpr const char* kSpoolmanStatusHandler = "helix_openams_spoolman_status";
constexpr const char* kLoad = openams::kCommandLoad;
constexpr const char* kUnload = openams::kCommandUnload;
constexpr const char* kCancel = openams::kCommandCancel;
constexpr const char* kReset = openams::kCommandReset;

const json* object_member(const json& object, const char* key) {
    auto it = object.find(key);
    return it != object.end() && it->is_object() ? &(*it) : nullptr;
}

/// A fault code in plain words; an unfamiliar code reads as the unit's own text.
std::string describe_fault(const std::string& code, const std::string& text) {
    if (code == "motor_drive_fault") {
        return lv_tr("Motor drive fault");
    }
    if (code == "motion_timeout") {
        return lv_tr("Motion timed out");
    }
    return !text.empty() ? text
                         : (!code.empty() ? code : std::string(lv_tr("Filament System Error")));
}

/// Put @p info's filament fields on @p slot, so get_slot_info returns them at
/// once rather than after the next frame.
void write_filament_fields(SlotInfo& slot, const SlotInfo& info) {
    slot.assign_filament_fields(info);
}

} // namespace

json AmsBackendOpenAms::required_status_objects(const PrinterDiscovery& hw) {
    json objects = json::object();
    if (hw.mmu_type() == AmsType::OPENAMS) {
        objects[openams::kManagerObject] =
            json::array({"api_version", "schema", "ready", "commands", "lanes", "units", "groups",
                         "devices", "lanes_by_fps"});
    }
    return objects;
}

AmsBackendOpenAms::AmsBackendOpenAms(IMoonrakerAPI* api, IMoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    system_info_.type = AmsType::OPENAMS;
    system_info_.type_name = "OpenAMS"; // i18n: do not translate - product name
    system_info_.version = "unknown";
    system_info_.supports_bypass = false;
}

// ============================================================================
// Lifecycle and status
// ============================================================================

void AmsBackendOpenAms::on_started() {
    if (!api_) {
        return;
    }
    // Blocks on the Moonraker DB, so no lock is held; the result is published
    // under the lock in one move.
    auto loaded =
        helix::ams::make_loaded_override_store(api_, "openams", get_type(), backend_log_tag());
    if (loaded.store) {
        helix::ams::ingest_legacy_records(*loaded.store, helix::ams::LegacyLockKeys::LaneData,
                                          backend_index());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        override_store_ = std::move(loaded.store);
        overrides_ = std::move(loaded.overrides);
        parse_snapshot_locked();
    }
    emit_event(EVENT_STATE_CHANGED);

    // The openams_spoolman component rewrites lane_data whenever a spool link
    // changes and announces it with this notification. A manager without the
    // component never sends it, and its links live in the override store.
    if (client_) {
        client_->register_method_callback(kSpoolmanStatusMethod, kSpoolmanStatusHandler,
                                          [this, token = lifetime_.token()](const json&) {
                                              token.defer("AmsBackendOpenAms::spoolman_status",
                                                          [this]() { refresh_lane_records(); });
                                          });
    }
}

void AmsBackendOpenAms::on_stopping() {
    if (client_) {
        client_->unregister_method_callback(kSpoolmanStatusMethod, kSpoolmanStatusHandler);
    }
}

void AmsBackendOpenAms::refresh_lane_records() {
    helix::ams::FilamentSlotOverrideStore* store = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        store = override_store_.get();
    }
    if (!store) {
        return;
    }
    const int block = backend_index();
    store->reload_async([this, block, token = lifetime_.token()](
                            std::unordered_map<int, helix::ams::LaneDataRecord> records) {
        token.defer(
            "AmsBackendOpenAms::apply_lane_records",
            [this, block, records = std::move(records)]() { apply_lane_records(block, records); });
    });
}

void AmsBackendOpenAms::apply_lane_records(
    int backend_block, const std::unordered_map<int, helix::ams::LaneDataRecord>& records) {
    for (const auto& [slot_index, entry] : records) {
        const helix::ams::LaneId lane = helix::ams::lane_id_for(backend_block, slot_index);
        helix::ams::LaneSources sources = helix::ams::sources_from_record(
            entry.record, entry.wire, helix::ams::LegacyLockKeys::LaneData);
        // A re-read never re-files a LocalUser statement out of our own record:
        // that would forge an edit. A spool link openams_spoolman wrote after
        // the last edit made here is the newer statement, and takes the user's
        // rung over it.
        sources.local_user.reset();
        bool write_in_flight = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            write_in_flight = own_write_echoes_.standing(slot_index);
        }
        helix::ams::file_lane_sources(lane, sources);
        helix::ams::file_outside_edit_if_newer(
            lane, entry, helix::ams::declared_from_record(entry.record), write_in_flight);
    }
    int total = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        total = system_info_.total_slots;
    }
    for (int slot = 0; slot < total; ++slot) {
        repaint_slot_from_lane(slot);
    }
    emit_event(EVENT_STATE_CHANGED);
}

void AmsBackendOpenAms::handle_status(const json& status) {
    const json* update = object_member(status, openams::kManagerObject);
    if (!update) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = update->begin(); it != update->end(); ++it) {
            snapshot_[it.key()] = it.value();
        }
        parse_snapshot_locked();
    }
    emit_event(EVENT_STATE_CHANGED);
}

void AmsBackendOpenAms::present_nothing_locked() {
    api_supported_ = false;
    manager_ready_ = false;
    reported_action_ = AmsAction::IDLE;
    lane_states_.clear();
    lane_ids_.clear();
    lane_loaded_slots_.clear();
    slot_lanes_.clear();
    unit_faults_.clear();
    unit_dryers_.clear();
    has_unit_climate_ = false;
    requested_dry_min_.clear();
    unit_oams_idx_.clear();
    present_by_slot_id_.clear();
    remote_slot_ids_.clear();
    slot_groups_.clear();
    groups_.clear();
    commands_.clear();
    topology_ = PathTopology::HUB;

    system_info_.units.clear();
    system_info_.total_slots = 0;
    system_info_.tool_to_slot_map.clear();
    system_info_.endless_spool_group_ids.clear();
    system_info_.endless_spool_groups_reported = false;
    system_info_.endless_spool_enabled = false;
    system_info_.current_slot = -1;
    system_info_.current_tool = -1;
    system_info_.filament_loaded = false;
    system_info_.action = pending_action_;
    system_info_.operation_detail.clear();
}

void AmsBackendOpenAms::parse_snapshot_locked() {
    openams::ManagerStatus parsed = openams::parse_manager_status(snapshot_);
    if (!parsed.supported) {
        if (api_supported_) {
            spdlog::warn("{} oams_manager stopped publishing a supported UI API; presenting "
                         "no slots",
                         backend_log_tag());
        }
        present_nothing_locked();
        return;
    }
    for (const auto& [action, command] : parsed.unsafe_commands) {
        spdlog::warn("{} Ignoring unsafe {} command name '{}'", backend_log_tag(), action, command);
    }
    for (const auto& [unit, topology] : parsed.unknown_topologies) {
        spdlog::debug("{} Unit '{}' has unknown topology '{}'; drawing it as a hub",
                      backend_log_tag(), unit, topology);
    }

    AmsSystemInfo next;
    next.type = AmsType::OPENAMS;
    next.type_name = system_info_.type_name;
    next.version = std::to_string(openams::kApiVersion);
    next.supports_bypass = false;
    next.units = std::move(parsed.units);
    next.total_slots = parsed.total_slots;
    next.filament_loaded = parsed.filament_loaded;
    next.current_slot = parsed.current_slot;
    next.current_tool = parsed.current_tool;

    reported_action_ = parsed.reported_action;
    lane_states_.clear();
    lane_ids_.clear();
    lane_loaded_slots_.clear();
    for (auto& lane : parsed.lanes) {
        lane_states_.push_back(std::move(lane.state));
        lane_ids_.push_back(std::move(lane.id));
        lane_loaded_slots_.push_back(lane.loaded_slot);
    }

    for (std::size_t u = 0; u < parsed.dryers.size(); ++u) {
        DryerInfo& info = parsed.dryers[u].info;
        const auto requested = requested_dry_min_.find(static_cast<int>(u));
        if (info.active) {
            info.duration_min = std::max(
                info.remaining_min, requested != requested_dry_min_.end() ? requested->second : 0);
        }
    }
    for (auto it = requested_dry_min_.begin(); it != requested_dry_min_.end();) {
        const auto u = static_cast<std::size_t>(it->first);
        it = (u < parsed.dryers.size() && parsed.dryers[u].info.active)
                 ? std::next(it)
                 : requested_dry_min_.erase(it);
    }

    bool any_fault = false;
    for (std::size_t u = 0; u < next.units.size(); ++u) {
        AmsUnit& unit = next.units[u];
        for (const UnitFault& fault : parsed.faults[u]) {
            any_fault = true;
            SlotError error;
            error.message = describe_fault(fault.code, fault.text);
            error.severity = SlotError::ERROR;
            for (auto& slot : unit.slots) {
                if (fault.bay < 0 || fault.bay == slot.slot_index) {
                    slot.error = error;
                }
            }
        }
    }

    remote_slot_ids_ = std::move(parsed.remote_slot_ids);
    slot_groups_ = std::move(parsed.slot_groups);
    slot_lanes_ = std::move(parsed.slot_lanes);
    unit_faults_ = std::move(parsed.faults);
    unit_dryers_ = std::move(parsed.dryers);
    has_unit_climate_ = std::any_of(unit_dryers_.begin(), unit_dryers_.end(),
                                    [](const UnitDryer& d) { return d.environment || d.offered; });
    unit_oams_idx_ = std::move(parsed.unit_oams_idx);
    groups_ = std::move(parsed.groups);
    commands_ = std::move(parsed.commands);
    manager_ready_ = parsed.ready;
    api_supported_ = true;
    topology_ = parsed.topology;

    // A T<n> group names the slots that can serve tool n. Exactly one of them
    // serves it now and carries the tool. The others are its runout backups and
    // carry none, so a Load on a backup dispatches that exact slot, not a tool
    // change that would load the serving slot instead.
    for (const auto& group : groups_) {
        const int tool = openams::tool_from_group(group.name);
        if (tool < 0 || group.slots.empty()) {
            continue;
        }
        const int serving = serving_slot_locked(group, next);
        if (tool >= static_cast<int>(next.tool_to_slot_map.size())) {
            next.tool_to_slot_map.resize(static_cast<std::size_t>(tool) + 1, -1);
        }
        next.tool_to_slot_map[static_cast<std::size_t>(tool)] = serving;
        for (int member : group.slots) {
            if (SlotInfo* slot = next.get_slot_global(member)) {
                slot->mapped_tool = member == serving ? tool : -1;
            }
        }
    }

    // A group of two or more slots is OpenAMS's runout backup, which the app
    // calls endless spool. The firmware config defines these groups, so they
    // are reported read-only. Held back until the manager is ready, so until
    // then the endless-spool defaults stand.
    if (manager_ready_) {
        std::vector<int> endless_ids(static_cast<std::size_t>(next.total_slots), -1);
        int endless_ordinal = 0;
        for (const auto& group : groups_) {
            if (group.slots.size() < 2) {
                continue;
            }
            for (int member : group.slots) {
                if (member >= 0 && member < next.total_slots) {
                    endless_ids[static_cast<std::size_t>(member)] = endless_ordinal;
                }
            }
            ++endless_ordinal;
        }
        next.endless_spool_group_ids = std::move(endless_ids);
        next.endless_spool_groups_reported = true;
        next.endless_spool_enabled = endless_ordinal > 0;
    }

    // OpenAMS reads nothing off a spool, so every insert is one the hardware
    // has no evidence about: the record stays and the user is asked
    // (docs/specs/filament_slots.md §6). Only an observed empty counts, so the
    // first frame after start is a baseline, not a wave of inserts. A unit
    // that is offline, or a manager that is not ready, reports bays it has not
    // read, so those frames neither raise an insert nor move the baseline.
    std::unordered_map<int, bool> present_now;
    for (const auto& unit : next.units) {
        const bool bays_read = manager_ready_ && unit.connected;
        for (const auto& slot : unit.slots) {
            const int slot_id = remote_slot_ids_[static_cast<std::size_t>(slot.global_index)];
            auto before = present_by_slot_id_.find(slot_id);
            if (!bays_read) {
                if (before != present_by_slot_id_.end()) {
                    present_now[slot_id] = before->second;
                }
                continue;
            }
            const bool present = slot.status != SlotStatus::EMPTY;
            if (present && before != present_by_slot_id_.end() && !before->second) {
                const int slot_index = slot.global_index;
                helix::ui::queue_update("AmsBackendOpenAms::parse_snapshot_locked", [slot_index] {
                    helix::ui::offer_clear_after_unverified_insert(slot_index);
                });
            }
            present_now[slot_id] = present;
        }
    }
    present_by_slot_id_ = std::move(present_now);

    for (auto& unit : next.units) {
        for (auto& slot : unit.slots) {
            apply_resolved_lane(slot, slot.global_index);
        }
    }

    next.pending_target_slot = pending_slot_;
    next.action = pending_action_ != AmsAction::IDLE ? pending_action_ : reported_action_;
    next.operation_detail = failure_detail_;
    system_info_ = std::move(next);
    if (system_info_.operation_detail.empty() && any_fault) {
        system_info_.operation_detail = fault_detail_locked();
    }
}

// ============================================================================
// State queries
// ============================================================================

AmsSystemInfo AmsBackendOpenAms::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_;
}

SlotInfo* AmsBackendOpenAms::cached_slot_locked(int slot_index) {
    return system_info_.get_slot_global(slot_index);
}

void AmsBackendOpenAms::prepare_lane_repaint_locked(int slot_index, SlotInfo& slot) {
    (void)slot_index;
    // The lane is the only supplier. overrides_ is not refreshed by a resync,
    // so restating a field from it would bring back what the lane has dropped.
    helix::ams::clear_lane_only_identity(slot, nullptr);
}

PathTopology AmsBackendOpenAms::get_topology() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return topology_;
}

PathTopology AmsBackendOpenAms::get_unit_topology(int unit_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit_index >= 0 && unit_index < static_cast<int>(system_info_.units.size())) {
        return system_info_.units[static_cast<std::size_t>(unit_index)].topology;
    }
    return topology_;
}

PathSegment AmsBackendOpenAms::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const SlotInfo* slot = system_info_.current_slot >= 0
                               ? system_info_.get_slot_global(system_info_.current_slot)
                               : nullptr;
    if (!slot) {
        return PathSegment::NONE;
    }
    if (slot->status == SlotStatus::LOADED) {
        return PathSegment::NOZZLE;
    }
    return slot->is_present() ? PathSegment::SPOOL : PathSegment::NONE;
}

PathSegment AmsBackendOpenAms::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const SlotInfo* slot = system_info_.get_slot_global(slot_index);
    if (!slot) {
        return PathSegment::NONE;
    }
    if (slot->status == SlotStatus::LOADED) {
        return PathSegment::NOZZLE;
    }
    if (slot_index == pending_slot_ && pending_action_ == AmsAction::LOADING) {
        return PathSegment::OUTPUT;
    }
    return slot->is_present() ? PathSegment::SPOOL : PathSegment::NONE;
}

PathSegment AmsBackendOpenAms::infer_error_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (system_info_.action != AmsAction::ERROR && reported_action_ != AmsAction::ERROR) {
        return PathSegment::NONE;
    }
    if (pending_slot_ >= 0) {
        return pending_action_ == AmsAction::LOADING ? PathSegment::OUTPUT : PathSegment::HUB;
    }
    return system_info_.filament_loaded ? PathSegment::NOZZLE : PathSegment::SPOOL;
}

bool AmsBackendOpenAms::can_unload_from_toolhead(int slot_index) const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (command_locked(kUnload).empty()) {
            return false;
        }
    }
    return AmsSubscriptionBackend::can_unload_from_toolhead(slot_index);
}

// ============================================================================
// Filament operations
// ============================================================================

AmsError AmsBackendOpenAms::manager_accepts_locked() const {
    if (!api_supported_) {
        return AmsErrorHelper::not_supported("OpenAMS without its UI API");
    }
    if (!manager_ready_) {
        return AmsErrorHelper::wrong_state("OpenAMS not ready", "ready");
    }
    return AmsErrorHelper::success();
}

std::string AmsBackendOpenAms::command_locked(const char* action) const {
    auto it = commands_.find(action);
    return it == commands_.end() ? std::string() : it->second;
}

bool AmsBackendOpenAms::slot_loadable_locked(int slot_index) const {
    const SlotInfo* slot = system_info_.get_slot_global(slot_index);
    return slot && (slot->status == SlotStatus::AVAILABLE || slot->status == SlotStatus::LOADED);
}

int AmsBackendOpenAms::serving_slot_locked(const Group& group, const AmsSystemInfo& info) const {
    // The loaded member first, so a tool change to the active tool stays on its
    // spool; otherwise the first member with a spool ready to feed.
    for (int member : group.slots) {
        const SlotInfo* slot = info.get_slot_global(member);
        if (slot && slot->status == SlotStatus::LOADED) {
            return member;
        }
    }
    for (int member : group.slots) {
        const SlotInfo* slot = info.get_slot_global(member);
        if (slot && slot->status == SlotStatus::AVAILABLE) {
            return member;
        }
    }
    // Nothing can feed: the first member names the slot a refusal reports.
    return group.slots.empty() ? -1 : group.slots.front();
}

int AmsBackendOpenAms::loaded_lane_count_locked() const {
    return static_cast<int>(std::count(lane_states_.begin(), lane_states_.end(), "loaded"));
}

std::string AmsBackendOpenAms::loaded_lane_of_slot_locked(int slot_index) const {
    if (slot_index < 0) {
        return {};
    }
    for (std::size_t i = 0; i < lane_loaded_slots_.size(); ++i) {
        if (lane_loaded_slots_[i] == slot_index) {
            return lane_ids_[i];
        }
    }
    return {};
}

int AmsBackendOpenAms::loaded_slot_on_lane_locked(const std::string& lane) const {
    for (std::size_t i = 0; i < lane_ids_.size(); ++i) {
        if (lane_ids_[i] == lane) {
            return lane_loaded_slots_[i];
        }
    }
    return -1;
}

bool AmsBackendOpenAms::needs_unload_before_load(const AmsSystemInfo& info, int target_slot) const {
    (void)info;
    std::lock_guard<std::mutex> lock(mutex_);
    if (target_slot < 0 || static_cast<std::size_t>(target_slot) >= slot_lanes_.size()) {
        return false;
    }
    const int loaded =
        loaded_slot_on_lane_locked(slot_lanes_[static_cast<std::size_t>(target_slot)]);
    return loaded >= 0 && loaded != target_slot;
}

AmsError AmsBackendOpenAms::load_gcode_locked(int slot_index, std::string& gcode) const {
    if (AmsError accepts = manager_accepts_locked(); !accepts.success()) {
        return accepts;
    }
    const std::string command = command_locked(kLoad);
    if (command.empty()) {
        return AmsErrorHelper::not_supported("OpenAMS load: this manager advertises no load "
                                             "command");
    }
    if (AmsError valid = validate_slot_index_locked(slot_index); !valid.success()) {
        return valid;
    }
    const std::string& group = slot_groups_[static_cast<std::size_t>(slot_index)];
    if (group.empty() || !IMoonrakerAPI::is_safe_gcode_param(group)) {
        return AmsErrorHelper::invalid_parameter("OpenAMS slot " + std::to_string(slot_index) +
                                                 " belongs to no usable filament group");
    }
    if (!slot_loadable_locked(slot_index)) {
        return AmsErrorHelper::slot_not_available(lane_noun(), slot_index);
    }
    gcode = command + " GROUP=" + IMoonrakerAPI::gcode_param_value(group) +
            " SLOT=" + std::to_string(remote_slot_ids_[static_cast<std::size_t>(slot_index)]);

    // The manager's load does not clear a lane that already holds another
    // slot, so that lane is unloaded first, in the same script.
    const std::string& lane = slot_lanes_[static_cast<std::size_t>(slot_index)];
    const int loaded = lane.empty() ? -1 : loaded_slot_on_lane_locked(lane);
    if (loaded >= 0 && loaded != slot_index) {
        const std::string unload = command_locked(kUnload);
        if (unload.empty()) {
            return AmsErrorHelper::not_supported(
                "OpenAMS swap: this manager advertises no unload command");
        }
        if (!IMoonrakerAPI::is_safe_gcode_param(lane)) {
            return AmsErrorHelper::invalid_parameter("OpenAMS lane '" + lane +
                                                     "' is not a usable gcode parameter");
        }
        gcode = unload + " FPS=" + IMoonrakerAPI::gcode_param_value(lane) + "\n" + gcode;
    }
    return AmsErrorHelper::success();
}

AmsError AmsBackendOpenAms::send_operation_gcode(
    const std::string& gcode, helix::SharedCallback<void()> on_complete,
    helix::SharedCallback<void(const MoonrakerError&)> on_error) {
    return ensure_homed_then(gcode, std::move(on_complete), std::move(on_error),
                             IMoonrakerAPI::kAmsOperationTimeoutMs,
                             /*skip_homing=*/true, /*silent=*/false,
                             /*caller_surfaces_errors=*/false);
}

AmsError AmsBackendOpenAms::begin_operation(AmsAction action, int slot_index,
                                            const std::string& gcode,
                                            const char* completion_event) {
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_action_ = action;
        pending_slot_ = slot_index;
        failure_detail_.clear();
        generation = ++operation_generation_;
        system_info_.action = action;
        system_info_.pending_target_slot = slot_index;
        system_info_.operation_detail.clear();
    }
    emit_event(EVENT_STATE_CHANGED);

    auto token = lifetime_.token();
    AmsError result = send_operation_gcode(
        gcode,
        [this, token, generation, completion_event]() {
            token.defer("AmsBackendOpenAms::operation_complete",
                        [this, generation, completion_event]() {
                            finish_operation(generation, completion_event);
                        });
        },
        [this, token, generation](const MoonrakerError& error) {
            token.defer("AmsBackendOpenAms::operation_error",
                        [this, generation, error]() { fail_operation(generation, error); });
        });
    if (!result.success()) {
        bool unwound = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation == operation_generation_) {
                pending_action_ = AmsAction::IDLE;
                pending_slot_ = -1;
                system_info_.action = reported_action_;
                system_info_.pending_target_slot = -1;
                unwound = true;
            }
        }
        if (unwound) {
            emit_event(EVENT_STATE_CHANGED);
        }
    }
    return result;
}

void AmsBackendOpenAms::finish_operation(std::uint64_t generation, const char* event) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != operation_generation_) {
            return;
        }
        pending_action_ = AmsAction::IDLE;
        pending_slot_ = -1;
        system_info_.action = reported_action_;
        system_info_.pending_target_slot = -1;
    }
    emit_event(EVENT_STATE_CHANGED);
    emit_event(event);
}

void AmsBackendOpenAms::fail_operation(std::uint64_t generation, const MoonrakerError& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != operation_generation_) {
            return;
        }
        pending_action_ = AmsAction::IDLE;
        pending_slot_ = -1;
        failure_detail_ = error.message;
        system_info_.action = reported_action_;
        system_info_.pending_target_slot = -1;
        system_info_.operation_detail = failure_detail_;
    }
    // The G-code error stream has already shown the macro's own message.
    spdlog::warn("{} Operation failed: {}", backend_log_tag(), error.message);
    emit_event(EVENT_STATE_CHANGED);
}

AmsError AmsBackendOpenAms::do_load_filament(int slot_index) {
    std::string gcode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (AmsError ready = load_gcode_locked(slot_index, gcode); !ready.success()) {
            return ready;
        }
    }
    return begin_operation(AmsAction::LOADING, slot_index, gcode, kEventLoadComplete);
}

AmsError AmsBackendOpenAms::do_unload_filament(int slot_index) {
    std::string command;
    int unload_slot = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (AmsError accepts = manager_accepts_locked(); !accepts.success()) {
            return accepts;
        }
        command = command_locked(kUnload);
        if (command.empty()) {
            return AmsErrorHelper::not_supported("OpenAMS unload: this manager advertises no "
                                                 "unload command");
        }
        // The unload names the lane the loaded slot sits on, so it empties
        // that lane even when several are loaded. A caller that names no
        // loaded slot gets the one lane that is loaded, if there is only one.
        unload_slot = slot_index;
        std::string lane = loaded_lane_of_slot_locked(unload_slot);
        if (lane.empty()) {
            unload_slot = system_info_.current_slot;
            lane = loaded_lane_of_slot_locked(unload_slot);
        }
        if (lane.empty() && loaded_lane_count_locked() == 1) {
            auto loaded = std::find_if(lane_loaded_slots_.begin(), lane_loaded_slots_.end(),
                                       [](int s) { return s >= 0; });
            if (loaded != lane_loaded_slots_.end()) {
                unload_slot = *loaded;
                lane = loaded_lane_of_slot_locked(unload_slot);
            }
        }
        if (lane.empty() || !IMoonrakerAPI::is_safe_gcode_param(lane)) {
            return AmsErrorHelper::invalid_parameter(
                "OpenAMS unload: no loaded slot names a usable lane");
        }
        command += " FPS=" + IMoonrakerAPI::gcode_param_value(lane);
    }
    return begin_operation(AmsAction::UNLOADING, unload_slot, command, kEventUnloadComplete);
}

AmsError AmsBackendOpenAms::do_select_slot(int slot_index) {
    (void)slot_index;
    return AmsErrorHelper::not_supported("OpenAMS slot selection without loading");
}

AmsError AmsBackendOpenAms::do_change_tool(int tool_number) {
    std::string gcode;
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (AmsError accepts = manager_accepts_locked(); !accepts.success()) {
            return accepts;
        }
        const std::string group_name = "T" + std::to_string(tool_number);
        auto group = std::find_if(groups_.begin(), groups_.end(),
                                  [&](const Group& g) { return g.name == group_name; });
        if (tool_number < 0 || group == groups_.end() || group->slots.empty()) {
            return AmsErrorHelper::tool_out_of_range(tool_number);
        }
        slot = serving_slot_locked(*group, system_info_);
        if (!slot_loadable_locked(slot)) {
            return AmsErrorHelper::slot_not_available(lane_noun(), group->slots.front());
        }
        if (AmsError ready = load_gcode_locked(slot, gcode); !ready.success()) {
            return ready;
        }
    }
    return begin_operation(AmsAction::LOADING, slot, gcode, kEventLoadComplete);
}

// ============================================================================
// Recovery
// ============================================================================

std::string AmsBackendOpenAms::clear_script_locked() const {
    std::string script;
    for (std::size_t u = 0; u < unit_faults_.size() && u < unit_oams_idx_.size(); ++u) {
        const bool clearable = std::any_of(unit_faults_[u].begin(), unit_faults_[u].end(),
                                           [](const UnitFault& f) { return f.clearable; });
        if (clearable && unit_oams_idx_[u] >= 0) {
            script += "OAMS_CLEAR_FAULT OAMS=" + std::to_string(unit_oams_idx_[u]) + "\n";
        }
    }
    const std::string reset = command_locked(kReset);
    script += reset;
    if (reset.empty() && !script.empty()) {
        script.pop_back(); // the trailing newline
    }
    return script;
}

bool AmsBackendOpenAms::has_clearable_fault_locked() const {
    return std::any_of(unit_faults_.begin(), unit_faults_.end(), [](const auto& faults) {
        return std::any_of(faults.begin(), faults.end(),
                           [](const UnitFault& f) { return f.clearable; });
    });
}

std::string AmsBackendOpenAms::fault_detail_locked() const {
    std::string detail;
    for (std::size_t u = 0; u < unit_faults_.size() && u < system_info_.units.size(); ++u) {
        for (const UnitFault& fault : unit_faults_[u]) {
            if (!detail.empty()) {
                detail += "; ";
            }
            const AmsUnit& unit = system_info_.units[u];
            detail += (unit.display_name.empty() ? unit.name : unit.display_name) + ": " +
                      describe_fault(fault.code, fault.text);
        }
    }
    return detail;
}

std::optional<helix::ErrorEvent> AmsBackendOpenAms::current_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string detail = fault_detail_locked();
    if (detail.empty()) {
        return std::nullopt;
    }
    std::vector<helix::RecoveryAction> actions;
    const std::string script = clear_script_locked();
    if (!script.empty()) {
        actions.push_back({lv_tr("Reset"), script, "openams::clear_fault", "primary"});
    } else {
        // A critical event with no action is a button-less dialog the user
        // cannot close; an empty gcode is the dismiss spelling.
        actions.push_back({lv_tr("OK"), "", "openams::dismiss", ""});
    }
    return helix::make_ams_fault_event(helix::ErrorSource::OPENAMS, lv_tr("Filament System Error"),
                                       detail, std::move(actions));
}

AmsError AmsBackendOpenAms::reset() {
    std::string script;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!api_supported_) {
            return AmsErrorHelper::not_supported("OpenAMS without its UI API");
        }
        script = clear_script_locked();
    }
    if (script.empty()) {
        return AmsErrorHelper::not_supported("OpenAMS reset");
    }
    return execute_gcode(script);
}

AmsError AmsBackendOpenAms::recover() {
    return reset();
}

void AmsBackendOpenAms::on_klipper_restarted() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_action_ = AmsAction::IDLE;
        pending_slot_ = -1;
        ++operation_generation_;
        system_info_.pending_target_slot = -1;
    }
    AmsSubscriptionBackend::on_klipper_restarted();
}

AmsError AmsBackendOpenAms::cancel() {
    std::string command;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!api_supported_) {
            return AmsErrorHelper::not_supported("OpenAMS without its UI API");
        }
        command = command_locked(kCancel);
        if (command.empty()) {
            return AmsErrorHelper::not_supported("OpenAMS cancel");
        }
        if (pending_action_ != AmsAction::IDLE) {
            return AmsErrorHelper::busy("an OpenAMS operation started from this screen");
        }
        if (std::find(lane_states_.begin(), lane_states_.end(), "loading") == lane_states_.end()) {
            return AmsErrorHelper::wrong_state("no OpenAMS load running", "loading");
        }
    }
    return execute_gcode(command);
}

bool AmsBackendOpenAms::can_cancel_operation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !command_locked(kCancel).empty() && pending_action_ == AmsAction::IDLE;
}

AmsError AmsBackendOpenAms::clear_fault(int slot_index) {
    (void)slot_index;
    std::string script;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_detail_.clear();
        system_info_.operation_detail.clear();
        // Only a latched unit fault is cleared from here; lane errors stay
        // with Reset.
        if (has_clearable_fault_locked()) {
            script = clear_script_locked();
        }
    }
    emit_event(EVENT_STATE_CHANGED);
    if (!script.empty()) {
        return execute_gcode(script);
    }
    return AmsErrorHelper::success();
}

// ============================================================================
// Slot identity
// ============================================================================

// ============================================================================
// Dryer
// ============================================================================

DryerInfo AmsBackendOpenAms::get_dryer_info(int unit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit < 0 || static_cast<std::size_t>(unit) >= unit_dryers_.size() ||
        !unit_dryers_[static_cast<std::size_t>(unit)].offered) {
        return DryerInfo{.supported = false};
    }
    return unit_dryers_[static_cast<std::size_t>(unit)].info;
}

AmsError AmsBackendOpenAms::start_drying(float temp_c, int duration_min, int fan_pct, int unit) {
    (void)fan_pct;
    std::string gcode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto u = static_cast<std::size_t>(unit);
        if (unit < 0 || u >= unit_dryers_.size() || !unit_dryers_[u].offered ||
            unit_oams_idx_.size() <= u || unit_oams_idx_[u] < 0) {
            return AmsErrorHelper::not_supported("Dryer");
        }
        const UnitDryer& dryer = unit_dryers_[u];
        if (!dryer.can_start) {
            return AmsError(AmsResult::WRONG_STATE, "Dryer is not accepting a start",
                            lv_tr("Dryer already running"),
                            lv_tr("Stop the running cycle before starting another"));
        }
        if (dryer.requires_unloaded && u < system_info_.units.size()) {
            for (const SlotInfo& slot : system_info_.units[u].slots) {
                if (slot.status == SlotStatus::LOADED) {
                    return AmsError(
                        AmsResult::WRONG_STATE, "Dryer needs every bay of this unit unloaded",
                        lv_tr("Unload this unit first"),
                        lv_tr("This dryer cannot run while filament from the unit is loaded"));
                }
            }
        }
        const float target = dryer.info.clamp_temp(temp_c);
        const int seconds = std::clamp(duration_min, 1, openams::kMaxDryerSeconds / 60) * 60;
        gcode = fmt::format("OAMS_DRYER_START OAMS={} TARGET={:g} DURATION={}", unit_oams_idx_[u],
                            target, seconds);
        requested_dry_min_[unit] = seconds / 60;
    }
    return execute_gcode(gcode);
}

AmsError AmsBackendOpenAms::stop_drying(int unit) {
    std::string gcode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto u = static_cast<std::size_t>(unit);
        if (unit < 0 || u >= unit_dryers_.size() || !unit_dryers_[u].offered ||
            unit_oams_idx_.size() <= u || unit_oams_idx_[u] < 0) {
            return AmsErrorHelper::not_supported("Dryer");
        }
        if (!unit_dryers_[u].can_stop) {
            return AmsError(AmsResult::WRONG_STATE, "Dryer is not accepting a stop",
                            lv_tr("Dryer is not running"), lv_tr("There is no cycle to stop"));
        }
        gcode = fmt::format("OAMS_DRYER_STOP OAMS={}", unit_oams_idx_[u]);
        requested_dry_min_.erase(unit);
    }
    return execute_gcode(gcode);
}

AmsError AmsBackendOpenAms::apply_user_edit(int slot_index, const SlotInfo& info,
                                            const helix::ams::Observation& declared) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SlotInfo* slot = system_info_.get_slot_global(slot_index);
        if (!slot) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index,
                                                system_info_.total_slots - 1);
        }
        write_filament_fields(*slot, info);
        helix::ams::stage_user_override(overrides_, slot_index, info, declared);
    }
    if (override_store_) {
        helix::ams::persist_staged_override(override_store_.get(), mutex_, overrides_, slot_index,
                                            backend_log_tag(), "Override");
    }
    emit_event(kEventSlotChanged, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

AmsError AmsBackendOpenAms::sync_external_identity(int slot_index, const SlotInfo& info) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SlotInfo* slot = system_info_.get_slot_global(slot_index);
        if (!slot) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index,
                                                system_info_.total_slots - 1);
        }
        write_filament_fields(*slot, info);
    }
    emit_event(kEventSlotChanged, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

void AmsBackendOpenAms::persist_slot_weight(int slot_index, float remaining_weight_g,
                                            float total_weight_g) {
    const std::string tag = backend_log_tag();
    std::lock_guard<std::mutex> lock(mutex_);
    helix::ams::persist_override_weight(override_store_.get(), overrides_, slot_index,
                                        remaining_weight_g, total_weight_g, tag);
}

void AmsBackendOpenAms::persist_external_identity_impl(int slot_index,
                                                       const helix::ams::Observation& spoolman) {
    const std::string tag = backend_log_tag();
    std::lock_guard<std::mutex> lock(mutex_);
    helix::ams::persist_override_external_identity(override_store_.get(), overrides_, slot_index,
                                                   spoolman, tag);
}

void AmsBackendOpenAms::clear_slot_override(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_override_locked(slot_index, system_info_.get_slot_global(slot_index));
    }
    emit_event(kEventSlotChanged, std::to_string(slot_index));
}

void AmsBackendOpenAms::clear_override_fields(SlotInfo& slot) const {
    AmsSubscriptionBackend::clear_override_fields(slot);
    slot.material.clear();
    slot.color_rgb = kAmsDefaultSlotColor;
    slot.multi_color_hexes.clear();
}

// ============================================================================
// Tool mapping and bypass
// ============================================================================

AmsError AmsBackendOpenAms::can_set_tool_mapping(int /*tool_number*/, int /*slot_index*/) const {
    return AmsErrorHelper::not_supported("OpenAMS tool mapping");
}

AmsError AmsBackendOpenAms::set_tool_mapping_impl(int tool_number, int slot_index) {
    return can_set_tool_mapping(tool_number, slot_index);
}

std::vector<int> AmsBackendOpenAms::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.tool_to_slot_map;
}

helix::FirmwareRouting AmsBackendOpenAms::firmware_default_routing() const {
    return helix::FirmwareRouting::from_table(get_tool_mapping());
}

helix::printer::EndlessSpoolCapabilities AmsBackendOpenAms::get_endless_spool_capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    using namespace helix::printer;
    // No groups reported yet (manager not ready, or API unsupported): the
    // base defaults, Unsupported and nothing enabled.
    if (!system_info_.endless_spool_groups_reported) {
        return {};
    }
    return {.availability = EndlessSpoolAvailability::Available,
            .enabled = system_info_.endless_spool_enabled ? EndlessSpoolEnabled::On
                                                          : EndlessSpoolEnabled::Off,
            .editability = EndlessSpoolEditability::ReadOnly,
            .restriction = EndlessSpoolRestriction::FirmwareManaged};
}

helix::printer::EndlessSpoolConfig AmsBackendOpenAms::get_endless_spool_config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return helix::printer::endless_spool_config_from_groups(system_info_.endless_spool_group_ids);
}

AmsError AmsBackendOpenAms::enable_bypass() {
    return AmsErrorHelper::not_supported("OpenAMS bypass");
}

AmsError AmsBackendOpenAms::disable_bypass() {
    return AmsErrorHelper::not_supported("OpenAMS bypass");
}

} // namespace helix
