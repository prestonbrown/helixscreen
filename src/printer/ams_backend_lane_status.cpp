// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_lane_status.h"

#include "ams_fault_event.h"
#include "filament_slot_override_store.h"
#include "i_moonraker_api.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "lvgl.h"
#include "moonraker_error.h"
#include "printer_discovery.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>

#include "hv/json.hpp"

namespace helix {
namespace {

using json = nlohmann::json;

/// Moonraker sends no database-change notification, so the records are polled.
constexpr std::chrono::seconds kRecordPollPeriod{10};

} // namespace

json AmsBackendLaneStatus::required_status_objects(const PrinterDiscovery& hw) {
    json objects = json::object();
    if (hw.mmu_type() == AmsType::LANE_STATUS) {
        objects[lane_status::kObject] =
            json::array({"spec_version", "producer", "kind", "wraps", "capabilities", "automap",
                         "compat_profiles", "units", "lanes", "current_lane", "state", "error"});
    }
    return objects;
}

AmsBackendLaneStatus::AmsBackendLaneStatus(IMoonrakerAPI* api, IMoonrakerClient* client)
    : AmsSubscriptionBackend(api, client), snapshot_(json::object()), records_(json::object()) {
    system_info_.type = AmsType::LANE_STATUS;
    system_info_.type_name = ams_type_to_string(AmsType::LANE_STATUS);
    system_info_.version = "unknown";
    system_info_.supports_bypass = false;
}

// ============================================================================
// Lifecycle and status
// ============================================================================

void AmsBackendLaneStatus::on_started() {
    read_records();
    if (lv_is_initialized()) {
        record_poll_.reset(lv_timer_create(
            [](lv_timer_t* timer) {
                static_cast<AmsBackendLaneStatus*>(lv_timer_get_user_data(timer))->read_records();
            },
            static_cast<uint32_t>(std::chrono::milliseconds(kRecordPollPeriod).count()), this));
    }
}

void AmsBackendLaneStatus::on_stopping() {
    record_poll_.reset();
}

void AmsBackendLaneStatus::handle_status(const json& status) {
    const auto update = status.find(lane_status::kObject);
    if (update == status.end() || !update->is_object()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = update->begin(); it != update->end(); ++it) {
            snapshot_[it.key()] = it.value();
        }
        rebuild_locked();
    }
    emit_event(EVENT_STATE_CHANGED);
}

void AmsBackendLaneStatus::read_records() {
    if (!api_) {
        return;
    }
    auto token = lifetime_.token();
    api_->database_get_namespace(
        ams::kLaneDataNamespace,
        [this, token](const json& doc) {
            token.defer("AmsBackendLaneStatus::adopt_records",
                        [this, doc]() { adopt_records(doc); });
        },
        [this, token](const MoonrakerError& err) {
            // A namespace nothing has written yet is empty, not unreachable.
            if (err.is_not_found()) {
                token.defer("AmsBackendLaneStatus::no_records",
                            [this]() { adopt_records(json::object()); });
                return;
            }
            spdlog::debug("[AMS LaneStatus] lane_data not read: {}", err.message);
        });
}

void AmsBackendLaneStatus::adopt_records(const json& namespace_doc) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const json next = namespace_doc.is_object() ? namespace_doc : json::object();
        if (next == records_) {
            return;
        }
        records_ = next;
        rebuild_locked();
    }
    emit_event(EVENT_STATE_CHANGED);
}

void AmsBackendLaneStatus::rebuild_locked() {
    const lane_status::Status parsed = lane_status::parse_status(snapshot_);
    if (!parsed.readable && !parsed.spec_version.empty() &&
        parsed.spec_version != status_.spec_version) {
        spdlog::warn("[AMS LaneStatus] lane_status spec_version {} is not one this screen reads; "
                     "showing no lanes",
                     parsed.spec_version);
    }
    status_ = parsed;
    const std::vector<std::string> no_lanes;
    const std::vector<std::string>& lanes = status_.readable ? status_.lanes : no_lanes;
    const auto records = lane_status::join_lane_records(records_, lanes, status_.producer_name);

    AmsSystemInfo next;
    next.type = AmsType::LANE_STATUS;
    next.type_name = !status_.producer_name.empty()
                         ? status_.producer_name
                         : std::string(ams_type_to_string(AmsType::LANE_STATUS));
    next.version = !status_.producer_version.empty() ? status_.producer_version : "unknown";
    next.supports_bypass = false;
    next.total_slots = static_cast<int>(lanes.size());
    next.current_slot = status_.lane_index(status_.current_lane);
    next.filament_loaded = next.current_slot >= 0;

    if (!lanes.empty()) {
        // lane_status names its units but not which lanes each holds, so the
        // lanes are drawn as one unit.
        AmsUnit unit;
        unit.unit_index = 0;
        unit.name = status_.units.size() == 1 ? status_.units.front() : next.type_name;
        unit.slot_count = next.total_slots;
        unit.first_slot_global_index = 0;
        unit.topology = PathTopology::HUB;
        for (int i = 0; i < next.total_slots; ++i) {
            const auto& record = records[static_cast<std::size_t>(i)];
            const ams::LaneId lane = lane_id(i);
            SlotInfo slot;
            slot.slot_index = i;
            slot.global_index = i;
            const bool current = i == next.current_slot;
            std::optional<bool> present = record ? record->loaded : std::nullopt;
            if (current) {
                present = true;
            }
            if (record) {
                const ams::FilamentSlotOverride& id = record->identity;
                ams::Observation firmware = ams::record_observation(id);
                firmware.source = ams::ObservationSource::Firmware;
                ams::ingest(lane, firmware);
                const ams::ResolvedTemps temps = ams::resolved_temps(id);
                slot.nozzle_temp_recommended = temps.nozzle_temp;
                slot.nozzle_temp_min = temps.nozzle_temp;
                slot.nozzle_temp_max = temps.nozzle_temp;
                slot.bed_temp = temps.bed_temp;
                slot.mapped_tool = record->tool;
            } else {
                ams::drop_lane_source(lane, ams::ObservationSource::Firmware);
            }
            if (present.has_value()) {
                ams::Observation sensed(ams::ObservationSource::Sensed);
                sensed.present = *present;
                ams::ingest(lane, sensed);
                slot.status = current    ? SlotStatus::LOADED
                              : *present ? SlotStatus::AVAILABLE
                                         : SlotStatus::EMPTY;
            } else {
                ams::drop_lane_source(lane, ams::ObservationSource::Sensed);
                slot.status = SlotStatus::UNKNOWN;
            }
            apply_resolved_lane(slot, i);
            unit.slots.push_back(std::move(slot));
        }
        next.units.push_back(std::move(unit));
    }
    for (int i = next.total_slots; i < filed_slots_; ++i) {
        ams::drop_lane_source(lane_id(i), ams::ObservationSource::Firmware);
        ams::drop_lane_source(lane_id(i), ams::ObservationSource::Sensed);
    }
    filed_slots_ = next.total_slots;

    next.action = status_.readable ? lane_status::action_for(status_) : AmsAction::IDLE;
    if (status_.error_active) {
        next.operation_detail = status_.error_message;
    }
    system_info_ = std::move(next);
}

// ============================================================================
// State queries
// ============================================================================

AmsSystemInfo AmsBackendLaneStatus::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_;
}

lane_status::Status AmsBackendLaneStatus::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::string AmsBackendLaneStatus::lane_name(int global_slot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.readable || global_slot < 0 ||
        global_slot >= static_cast<int>(status_.lanes.size())) {
        return {};
    }
    return status_.lanes[static_cast<std::size_t>(global_slot)];
}

PathSegment AmsBackendLaneStatus::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.filament_loaded ? PathSegment::NOZZLE : PathSegment::NONE;
}

PathSegment AmsBackendLaneStatus::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const SlotInfo* slot = system_info_.get_slot_global(slot_index);
    if (!slot) {
        return PathSegment::NONE;
    }
    if (slot->status == SlotStatus::LOADED) {
        return PathSegment::NOZZLE;
    }
    return slot->is_present() ? PathSegment::SPOOL : PathSegment::NONE;
}

PathSegment AmsBackendLaneStatus::infer_error_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (system_info_.action != AmsAction::ERROR) {
        return PathSegment::NONE;
    }
    return system_info_.filament_loaded ? PathSegment::NOZZLE : PathSegment::SPOOL;
}

std::optional<helix::ErrorEvent> AmsBackendLaneStatus::current_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.readable || !status_.error_active) {
        return std::nullopt;
    }
    std::vector<helix::RecoveryAction> actions;
    // A critical event with no action is a dialog the user cannot close; an
    // empty gcode is the dismiss spelling.
    actions.push_back({lv_tr("OK"), "", "lane_status::dismiss", ""});
    const std::string detail = !status_.error_message.empty()
                                   ? status_.error_message
                                   : std::string(lv_tr("Filament System Error"));
    return helix::make_ams_fault_event(helix::ErrorSource::GENERIC, lv_tr("Filament System Error"),
                                       detail, std::move(actions));
}

// ============================================================================
// Filament operations
// ============================================================================

AmsError AmsBackendLaneStatus::offered_locked(const char* capability, const char* what) const {
    if (!status_.offers(capability)) {
        return AmsErrorHelper::not_supported(
            fmt::format("{} on {}: the producer does not offer it", what, system_info_.type_name));
    }
    return AmsErrorHelper::success();
}

bool AmsBackendLaneStatus::can_unload_from_toolhead(int slot_index) const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!status_.offers(lane_status::kCapabilityUnload)) {
            return false;
        }
    }
    return AmsSubscriptionBackend::can_unload_from_toolhead(slot_index);
}

AmsError AmsBackendLaneStatus::do_load_filament(int slot_index) {
    std::string gcode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (AmsError offered = offered_locked(lane_status::kCapabilityLoad, "Load");
            !offered.success()) {
            return offered;
        }
        if (AmsError valid = validate_slot_index_locked(slot_index); !valid.success()) {
            return valid;
        }
        // Lane names passed is_lane_name(), so each is a safe G-code value.
        gcode = "CHANGE_TOOL LANE=" + status_.lanes[static_cast<std::size_t>(slot_index)];
    }
    return execute_gcode(gcode);
}

AmsError AmsBackendLaneStatus::do_unload_filament(int slot_index) {
    std::string gcode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (AmsError offered = offered_locked(lane_status::kCapabilityUnload, "Unload");
            !offered.success()) {
            return offered;
        }
        // The loaded lane when the caller names none.
        const int slot = slot_index >= 0 ? slot_index : system_info_.current_slot;
        if (AmsError valid = validate_slot_index_locked(slot); !valid.success()) {
            return valid;
        }
        gcode = "TOOL_UNLOAD LANE=" + status_.lanes[static_cast<std::size_t>(slot)];
    }
    return execute_gcode(gcode);
}

AmsError AmsBackendLaneStatus::do_select_slot(int /*slot_index*/) {
    return AmsErrorHelper::not_supported("Lane selection without loading");
}

AmsError AmsBackendLaneStatus::do_change_tool(int /*tool_number*/) {
    return AmsErrorHelper::not_supported("Tool change through lane_status");
}

AmsError AmsBackendLaneStatus::recover() {
    return AmsErrorHelper::not_supported("Recovery through lane_status");
}

AmsError AmsBackendLaneStatus::reset() {
    return AmsErrorHelper::not_supported("Reset through lane_status");
}

AmsError AmsBackendLaneStatus::cancel() {
    return AmsErrorHelper::not_supported("Cancel through lane_status");
}

// ============================================================================
// Identity: the producer's records, read only
// ============================================================================

AmsError AmsBackendLaneStatus::apply_user_edit(int slot_index, const SlotInfo& /*info*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    return AmsErrorHelper::not_supported(
        fmt::format("Editing lane {} on {}: its producer owns the lane records", slot_index,
                    system_info_.type_name));
}

void AmsBackendLaneStatus::clear_slot_override(int slot_index) {
    spdlog::info("[AMS LaneStatus] Clear of slot {} skipped: the producer owns the lane records",
                 slot_index);
}

std::string AmsBackendLaneStatus::clear_spool_refusal(int /*slot_index*/) const {
    return lv_tr("This filament system keeps its own spool records");
}

void AmsBackendLaneStatus::persist_slot_weight(int slot_index, float /*remaining_weight_g*/,
                                               float /*total_weight_g*/) {
    spdlog::debug("[AMS LaneStatus] weight of slot {} not written: the producer owns it",
                  slot_index);
}

void AmsBackendLaneStatus::persist_external_identity_impl(int slot_index,
                                                          const ams::Observation& /*spoolman*/) {
    spdlog::debug("[AMS LaneStatus] Spoolman identity of slot {} not written: the producer owns "
                  "it",
                  slot_index);
}

AmsError AmsBackendLaneStatus::can_set_tool_mapping(int /*tool_number*/, int /*slot_index*/) const {
    return AmsErrorHelper::not_supported("Tool mapping through lane_status");
}

AmsError AmsBackendLaneStatus::enable_bypass() {
    return AmsErrorHelper::not_supported("Bypass through lane_status");
}

AmsError AmsBackendLaneStatus::disable_bypass() {
    return AmsErrorHelper::not_supported("Bypass through lane_status");
}

} // namespace helix
