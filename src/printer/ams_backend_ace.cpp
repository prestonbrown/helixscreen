// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_ACE

/**
 * @file ams_backend_ace.cpp
 * @brief ACE (AnyCubic ACE Pro) backend implementation
 *
 * Primary path: WebSocket subscription to ace Klipper object (ValgACE).
 * Fallback path: REST polling via /server/ace/ endpoints (BunnyACE/DuckACE).
 * See ams_backend_ace.h for full documentation.
 */

#include "ams_backend_ace.h"

#include "ui_insert_notice.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "ams_bypass_policy.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lane_apply.h"
#include "lane_legacy_migration.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "post_op_cooldown_manager.h"
#include "settings_manager.h"
#include "spdlog/spdlog.h"

#include <spdlog/fmt/fmt.h>

#include <chrono>

namespace helix {

using json = nlohmann::json;

/// Max consecutive /server/ace/info failures before giving up on REST fallback
static constexpr int MAX_INFO_FETCH_FAILURES = 3;

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsBackendAce::AmsBackendAce(IMoonrakerAPI* api, IMoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Initialize system info with ACE defaults
    system_info_.type = AmsType::ACE;
    system_info_.type_name = "ACE";
    system_info_.supports_bypass = false;

    // Initialize dryer info with ACE Pro capabilities
    dryer_info_.supported = true;
    dryer_info_.active = false;
    dryer_info_.allows_during_print = false;
    dryer_info_.min_temp_c = 35.0f;
    dryer_info_.max_temp_c = 55.0f;
    dryer_info_.max_duration_min = 720; // 12 hours
    dryer_info_.supports_fan_control = false;
    // Both live-adjust flags keep their conservative defaults: ACE_START_DRYING and
    // ACE_STOP_DRYING are the whole surface, with no set-temperature-while-running
    // command, so every adjustment stops and restarts the cycle.
}

AmsBackendAce::~AmsBackendAce() {
    // lifetime_ destructor calls invalidate() automatically
    stop_rest_fallback();
}

// ============================================================================
// AmsSubscriptionBackend Hooks
// ============================================================================

void AmsBackendAce::on_started() {
    spdlog::info("[ACE] Backend started — querying initial filament_hub/ace state via WebSocket");

    // A restart re-subscribes and re-parses from a fresh initial query, so
    // the insert edge re-baselines too: a prev status carried across a
    // disconnect would judge the first post-reconnect frame as an insert,
    // and an evidence map carried across a printer swap would compare
    // spools that never shared a bay.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prev_slot_status_.clear();
        last_spool_evidence_.clear();
        pending_insert_reads_.clear();
    }

    // Load persisted per-slot overrides from the shared FilamentSlotOverrideStore
    // BEFORE issuing the initial status query — otherwise the first status
    // callback (libhv background thread) could fire and parse slots before
    // overrides_ is populated, so the first EVENT_STATE_CHANGED frame would
    // miss override data. load_blocking runs on this (main) thread; the
    // Moonraker DB callback fires on the libhv event loop, so the two threads
    // don't interfere. Migration from helix-screen:ace_slot_overrides to
    // lane_data happens automatically inside load_blocking the first time
    // lane_data is empty (Task 8).
    if (api_) {
        auto loaded =
            helix::ams::make_loaded_override_store(api_, "ace", get_type(), backend_log_tag());
        if (loaded.store) {
            helix::ams::ingest_legacy_records(*loaded.store, helix::ams::LegacyLockKeys::LaneData,
                                              backend_index());
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            override_store_ = std::move(loaded.store);
            overrides_ = std::move(loaded.overrides);
        }
    }

    // start() refuses to run without a client, but a directly-constructed
    // backend (tests) reaches here with none and the query below would
    // dereference null (#1650).
    if (!client_) {
        spdlog::debug("[ACE] No Moonraker client — skipping initial status query");
        return;
    }

    auto token = lifetime_.token();

    // Query all known Klipper object names directly (works if driver has
    // get_status()). Native Anycubic GoKlipper registers the object as
    // `filament_hub`; community ValgACE/BunnyACE/DuckACE register it as `ace`;
    // the Kobra S1 mainline-Python fork registers each unit as
    // `ace_instance_N` (#1107). printer.objects.query tolerates unknown
    // objects (absent from the result) and select_ace_object handles
    // absent keys, so over-querying is safe.
    json objects_to_query = json::object();
    objects_to_query["filament_hub"] = nullptr;
    objects_to_query["ace"] = nullptr;
    for (int i = 0; i < 4; ++i) {
        objects_to_query[fmt::format("ace_instance_{}", i)] = nullptr;
    }

    json params = {{"objects", objects_to_query}};

    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this, token](const json& response) {
            // L081 Mechanism C: defer all member access (parse_ace_object,
            // info_fetched_, emit_event, start_rest_fallback) to main thread.
            // `this` capture is safe because the defer body checks the token
            // and skips if the owner has been destroyed.
            token.defer("AmsBackendAce::on_started_query", [this, response]() {
                // Prefer the native `filament_hub` key; fall back to the
                // community `ace` key. Commit to the subscription path ONLY
                // when the matched object actually carries slot data — a
                // manager-only object (Kobra S1 fork: ace_instances/current_index,
                // no slots) must fall through to the REST bridge (#1069).
                const json* ace_data = nullptr;
                std::string matched_key;
                if (response.contains("result") && response["result"].contains("status")) {
                    ace_data = select_ace_object(response["result"]["status"], &matched_key);
                }

                if (ace_data) {
                    // Native/ValgACE path: got real data from get_status()
                    spdlog::info("[ACE] Klipper '{}' object has status data — "
                                 "using native WebSocket subscription",
                                 matched_key);

                    parse_ace_object(*ace_data);
                    // A manager-shaped `ace` can ride the same response and
                    // states the seat (current_index); parse it AFTER the
                    // slot-bearing object so the seat stamps onto populated
                    // slots (#1069).
                    if (const json* manager = manager_ace_object(response["result"]["status"])) {
                        parse_ace_object(*manager);
                    }
                    info_fetched_.store(true);
                    emit_event(EVENT_STATE_CHANGED);

                } else {
                    // BunnyACE/DuckACE path: no get_status(), try REST fallback
                    spdlog::info("[ACE] Klipper filament_hub/ace object has no status data — "
                                 "trying REST bridge fallback (/server/ace/*)");
                    start_rest_fallback();
                }
            });
        },
        [this, token](const MoonrakerError& err) {
            token.defer("AmsBackendAce::on_started_query_err", [this, err]() {
                spdlog::warn("[ACE] Initial filament_hub/ace query failed: {} — "
                             "trying REST fallback",
                             err.message);
                start_rest_fallback();
            });
        });
}

void AmsBackendAce::on_stopping() {
    // on_stopping() is called with mutex_ held — do NOT lock mutex_ here.
    // stop_rest_fallback uses its own rest_stop_mutex_, which is safe.
    stop_rest_fallback();
    lifetime_.invalidate();
}

void AmsBackendAce::handle_status_update(const json& notification) {
    if (use_rest_fallback_)
        return; // Using REST polling, ignore subscriptions

    // notify_status_update format: {"params": [{...}, timestamp]}
    const json* status = &notification;
    if (notification.contains("params") && notification["params"].is_array() &&
        !notification["params"].empty()) {
        status = &notification["params"][0];
    }
    if (!status->is_object())
        return;

    // Native Anycubic GoKlipper publishes under `filament_hub`; community
    // ValgACE under `ace`; the Kobra S1 mainline-Python fork under
    // `ace_instance_N` with a manager-shaped `ace` beside it (#1069, #1107).
    //
    // A frame carrying any `ace_instance_N` key is the fork: the display is
    // anchored to the LOWEST instance (the one on_started's slot-bearing pick
    // populated), so that instance's delta wins even when it carries no slots
    // array while a higher instance's delta does — otherwise a multi-unit rig
    // would write another unit's inventory into the displayed one. The manager
    // is parsed after the primary, so the seat (current_index) lands on slots
    // that are already populated.
    //
    // Otherwise a slot-bearing object is parsed first; notify frames carry
    // only CHANGED fields, so a delta with no slots array at all falls back to
    // the first non-empty object (the manager is excluded there via `skip`).
    const json* manager = manager_ace_object(*status);
    const json* ace_data = nullptr;
    if (const std::string* instance_key = lowest_ace_instance_key(*status)) {
        ace_data = &(*status)[*instance_key];
    } else {
        ace_data = select_ace_object(*status, nullptr, /*require_slots=*/true);
        if (!ace_data) {
            ace_data = select_ace_object(*status, nullptr, /*require_slots=*/false, manager);
        }
    }

    if (!ace_data && !manager)
        return;

    if (ace_data) {
        parse_ace_object(*ace_data);
    }
    if (manager) {
        parse_ace_object(*manager);
    }
    emit_event(EVENT_STATE_CHANGED);
}

// ============================================================================
// State Queries
// ============================================================================

AmsSystemInfo AmsBackendAce::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_;
}

SlotInfo AmsBackendAce::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (system_info_.units.empty()) {
        SlotInfo empty;
        empty.slot_index = -1;
        empty.global_index = -1;
        return empty;
    }

    const auto& unit = system_info_.units[0];
    if (slot_index < 0 || slot_index >= static_cast<int>(unit.slots.size())) {
        SlotInfo empty;
        empty.slot_index = -1;
        empty.global_index = -1;
        return empty;
    }
    return unit.slots[static_cast<size_t>(slot_index)];
}

// ============================================================================
// Path Visualization
// ============================================================================

PathTopology AmsBackendAce::get_topology() const {
    return PathTopology::HUB;
}

PathSegment AmsBackendAce::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // A seated tool is the whole answer, and outranks the sensors: both are
    // still made with filament in the nozzle.
    if (system_info_.filament_loaded) {
        return PathSegment::NOZZLE;
    }

    // Nothing seated, so anything the sensors see is a strand in flight.
    if (path_sensors_seen_) {
        if (toolhead_sensor_) {
            return PathSegment::TOOLHEAD;
        }
        if (rdm_sensor_) {
            return PathSegment::OUTPUT;
        }
    }

    return PathSegment::NONE;
}

PathSegment AmsBackendAce::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (system_info_.units.empty()) {
        return PathSegment::NONE;
    }

    const auto& unit = system_info_.units[0];
    if (slot_index < 0 || slot_index >= static_cast<int>(unit.slots.size())) {
        return PathSegment::NONE;
    }

    const auto& slot = unit.slots[static_cast<size_t>(slot_index)];

    if (system_info_.filament_loaded && system_info_.current_slot == slot_index) {
        return PathSegment::NOZZLE;
    }

    if (slot.status == SlotStatus::AVAILABLE || slot.status == SlotStatus::LOADED) {
        return PathSegment::SPOOL;
    }

    return PathSegment::NONE;
}

PathSegment AmsBackendAce::infer_error_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (system_info_.action == AmsAction::ERROR) {
        return PathSegment::HUB;
    }

    return PathSegment::NONE;
}

// ============================================================================
// Filament Operations
// ============================================================================

AmsError AmsBackendAce::do_load_filament(int slot_index) {
    auto err = validate_slot_index(slot_index);
    if (!err.success()) {
        return err;
    }

    spdlog::info("[ACE] Loading filament from slot {}", slot_index);

    // Set action optimistically so sidebar detects the LOADING → IDLE transition.
    // The ACE Klipper module may not report "status": "loading" during gcode execution,
    // leaving the sidebar stuck waiting for a transition that never starts.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::LOADING;
    }
    emit_event(EVENT_STATE_CHANGED);

    std::string gcode = "ACE_CHANGE_TOOL TOOL=" + std::to_string(slot_index);
    auto token = lifetime_.token();

    return execute_gcode(
        gcode,
        [this, token, slot_index]() {
            // L081 Mechanism C: marshal member writes (system_info_) to main.
            token.defer("AmsBackendAce::load_done", [this, slot_index]() {
                spdlog::info("[ACE] Load slot {} gcode completed", slot_index);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    system_info_.action = AmsAction::IDLE;

                    // Where firmware states the seat itself, the ack is only
                    // "the command was accepted" and the seat waits for the
                    // driver to confirm it moved.
                    if (!manager_states_seat_) {
                        seat_from_local_index_locked(slot_index);

                        // Same derivation the parse paths use, so the next
                        // status frame re-applies this stamp instead of
                        // erasing it.
                        apply_seated_slot_stamp_locked();
                    }
                }
                PostOpCooldownManager::instance().schedule();
                emit_event(EVENT_STATE_CHANGED);
            });
        },
        [this, token](const MoonrakerError&) {
            // The send failed, so the optimistic LOADING has to be unwound or
            // the sidebar waits on a transition that never starts.
            token.defer("AmsBackendAce::load_err", [this]() {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    system_info_.action = AmsAction::IDLE;
                }
                emit_event(EVENT_STATE_CHANGED);
            });
        },
        /*silent=*/false);
}

AmsError AmsBackendAce::do_unload_filament(int /*slot_index*/) {
    spdlog::info("[ACE] Unloading filament");

    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::UNLOADING;
    }
    emit_event(EVENT_STATE_CHANGED);

    std::string gcode = "ACE_CHANGE_TOOL TOOL=-1";
    auto token = lifetime_.token();

    return execute_gcode(
        gcode,
        [this, token]() {
            // L081 Mechanism C: marshal member writes (system_info_) to main.
            token.defer("AmsBackendAce::unload_done", [this]() {
                spdlog::info("[ACE] Unload gcode completed");
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    system_info_.action = AmsAction::IDLE;

                    // Symmetric with the load path: a driver that states the
                    // seat also clears it, and an unload it declined would
                    // otherwise read as an empty toolhead.
                    if (!manager_states_seat_) {
                        seat_from_local_index_locked(-1);

                        // Releases the stamp back to the status the parse
                        // wrote, rather than assuming AVAILABLE for a slot
                        // firmware may have called EMPTY.
                        apply_seated_slot_stamp_locked();
                    }
                }
                PostOpCooldownManager::instance().schedule();
                emit_event(EVENT_STATE_CHANGED);
            });
        },
        [this, token](const MoonrakerError&) {
            // The send failed, so the optimistic UNLOADING has to be unwound or
            // the sidebar waits on a transition that never starts.
            token.defer("AmsBackendAce::unload_err", [this]() {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    system_info_.action = AmsAction::IDLE;
                }
                emit_event(EVENT_STATE_CHANGED);
            });
        },
        /*silent=*/false);
}

AmsError AmsBackendAce::do_select_slot(int slot_index) {
    return do_load_filament(slot_index);
}

AmsError AmsBackendAce::do_change_tool(int tool_number) {
    return do_load_filament(tool_number);
}

// ============================================================================
// Recovery Operations
// ============================================================================

AmsError AmsBackendAce::recover() {
    spdlog::info("[ACE] Attempting recovery");
    return execute_gcode("ACE_RECOVER");
}

AmsError AmsBackendAce::reset() {
    spdlog::info("[ACE] Resetting");
    return execute_gcode("ACE_RESET");
}

AmsError AmsBackendAce::cancel() {
    spdlog::info("[ACE] Cancelling operation");

    // Invalidate outstanding load/unload callbacks so they don't
    // overwrite state after cancel completes
    lifetime_.invalidate();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::IDLE;
    }
    emit_event(EVENT_STATE_CHANGED);

    return execute_gcode("ACE_CHANGE_TOOL TOOL=-1");
}

// ============================================================================
// Configuration
// ============================================================================

namespace {

/// Parse passes an insert whose tag read has not landed stays pending before
/// the no-read fallback verdict runs. The REST poll brings every bay through
/// the parse each POLL_INTERVAL_MS, so this bounds the wait on that path; the
/// WebSocket path is change-driven, where a read that never lands can leave
/// the insert pending until the next frame of any kind arrives - harmless,
/// since a late verdict is still judged against the occupant that was in the
/// bay when the spool went in.
constexpr int kAcePendingReadParsePasses = 8;

/// Put @p info's resolver-owned filament fields on @p slot so get_slot_info
/// returns them at once. The catalog product identity rides along: a sync that
/// dropped it would make the editor snap back to a different variant on the
/// next get_slot_info().
void write_filament_fields(SlotInfo& slot, const SlotInfo& info) {
    helix::ams::copy_resolver_owned_identity(slot, info);
}

} // namespace

AmsError AmsBackendAce::apply_user_edit(int slot_index, const SlotInfo& info,
                                        const helix::ams::Observation& declared) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        SlotInfo* slot = mutable_slot_locked(slot_index);
        if (!slot) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, 0);
        }
        write_filament_fields(*slot, info);

        // Stage the override into overrides_ so the edit survives a restart;
        // the lane's own declaration, filed when the edit is committed, is what
        // apply_resolved_lane paints on every subsequent parse.
        helix::ams::stage_user_override(overrides_, slot_index, info, declared);
    }

    spdlog::info("[ACE] Updated slot {} info: {} {}", slot_index, info.material, info.color_name);

    if (override_store_) {
        helix::ams::persist_staged_override(override_store_.get(), mutex_, overrides_, slot_index,
                                            backend_log_tag(), "Override");
    }

    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

AmsError AmsBackendAce::sync_external_identity(int slot_index, const SlotInfo& info) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        SlotInfo* slot = mutable_slot_locked(slot_index);
        if (!slot) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index, 0);
        }
        // overrides_ is left alone: a synced value lives in memory only, and
        // the next firmware parse overwrites it.
        write_filament_fields(*slot, info);
    }

    spdlog::info("[ACE] Synced slot {} info: {} {}", slot_index, info.material, info.color_name);
    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

void AmsBackendAce::persist_external_identity_impl(int slot_index,
                                                   const helix::ams::Observation& spoolman) {
    const std::string tag = backend_log_tag();
    std::lock_guard<std::mutex> lock(mutex_);
    helix::ams::persist_override_external_identity(override_store_.get(), overrides_, slot_index,
                                                   spoolman, tag);
}

void AmsBackendAce::persist_slot_weight(int slot_index, float remaining_weight_g,
                                        float total_weight_g) {
    const std::string tag = backend_log_tag();
    std::lock_guard<std::mutex> lock(mutex_);
    helix::ams::persist_override_weight(override_store_.get(), overrides_, slot_index,
                                        remaining_weight_g, total_weight_g, tag);
}

AmsError AmsBackendAce::set_tool_mapping_impl(int tool_number, int slot_index) {
    (void)tool_number;
    (void)slot_index;
    return AmsErrorHelper::not_supported("Tool mapping");
}

std::vector<int> AmsBackendAce::get_tool_mapping() const {
    return {};
}

// ============================================================================
// Bypass Mode (not supported)
// ============================================================================

void AmsBackendAce::set_bypass_macros(helix::BypassMacros macros) {
    std::lock_guard<std::mutex> lock(mutex_);
    bypass_on_macro_ = std::move(macros.on);
    bypass_off_macro_ = std::move(macros.off);
    if (ace_pro_enabled_seen_) {
        system_info_.supports_bypass = !bypass_on_macro_.empty() && !bypass_off_macro_.empty();
    }
}

AmsError AmsBackendAce::enable_bypass() {
    std::string gcode;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (AmsError precondition = check_preconditions(); !precondition) {
            return precondition;
        }
        // Checked before the availability predicate, which folds in a user
        // override that can force the controls on. That override cannot supply
        // a macro, and without one there is nothing to send.
        if (bypass_on_macro_.empty()) {
            return AmsError(AmsResult::WRONG_STATE, "Bypass not supported",
                            lv_tr("This system does not support bypass mode"), "");
        }
        if (!helix::bypass_available_for(system_info_.supports_bypass)) {
            return AmsError(AmsResult::WRONG_STATE, "Bypass not supported",
                            lv_tr("This system does not support bypass mode"), "");
        }
        // The macro refuses a seated tool itself, but execute_gcode is
        // fire-and-forget and reports success before Klipper answers, so a
        // refusal there would reach the user as a toast contradicting a success
        // already shown.
        if (system_info_.filament_loaded) {
            return AmsError(AmsResult::WRONG_STATE, "Unload filament first",
                            lv_tr("Filament is still loaded. Unload it before enabling bypass."),
                            "");
        }
        gcode = bypass_on_macro_;
    }

    spdlog::info("[ACE] Enabling bypass mode");
    return execute_gcode(gcode);
}

AmsError AmsBackendAce::disable_bypass() {
    std::string gcode;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (AmsError precondition = check_preconditions(); !precondition) {
            return precondition;
        }
        if (bypass_off_macro_.empty()) {
            return AmsError(AmsResult::WRONG_STATE, "Bypass not supported",
                            lv_tr("This system does not support bypass mode"), "");
        }
        if (!helix::bypass_available_for(system_info_.supports_bypass)) {
            return AmsError(AmsResult::WRONG_STATE, "Bypass not supported",
                            lv_tr("This system does not support bypass mode"), "");
        }
        gcode = bypass_off_macro_;
    }

    spdlog::info("[ACE] Disabling bypass mode");
    return execute_gcode(gcode);
}

bool AmsBackendAce::is_bypass_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Only the ACE path being switched off means a hand-fed spool. A rig that
    // never publishes the switch is not bypassing, it simply has no switch.
    return ace_pro_enabled_seen_ && !ace_pro_enabled_;
}

// ============================================================================
// Dryer Control
// ============================================================================

DryerInfo AmsBackendAce::get_dryer_info(int unit) const {
    (void)unit; // ACE is single-unit
    std::lock_guard<std::mutex> lock(mutex_);
    return dryer_info_;
}

AmsError AmsBackendAce::start_drying(float temp_c, int duration_min, int fan_pct, int unit) {
    (void)unit;
    auto err = check_preconditions();
    if (!err.success()) {
        return err;
    }

    float min_temp, max_temp;
    int max_duration;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        min_temp = dryer_info_.min_temp_c;
        max_temp = dryer_info_.max_temp_c;
        max_duration = dryer_info_.max_duration_min;
    }

    if (temp_c < min_temp || temp_c > max_temp) {
        return AmsError(AmsResult::COMMAND_FAILED,
                        "Temperature out of range: " + std::to_string(temp_c),
                        lv_tr("Invalid temperature"),
                        fmt::format(lv_tr("Set temperature between {}°C and {}°C"),
                                    static_cast<int>(min_temp), static_cast<int>(max_temp)));
    }

    if (duration_min <= 0 || duration_min > max_duration) {
        return AmsError(AmsResult::COMMAND_FAILED,
                        "Duration out of range: " + std::to_string(duration_min),
                        lv_tr("Invalid duration"),
                        fmt::format(lv_tr("Set duration between 1 and {} minutes"), max_duration));
    }

    spdlog::info("[ACE] Starting drying: {}°C for {} minutes", temp_c, duration_min);

    (void)fan_pct;

    std::string gcode = "ACE_START_DRYING TEMP=" + std::to_string(static_cast<int>(temp_c)) +
                        " DURATION=" + std::to_string(duration_min);
    return execute_gcode(gcode);
}

AmsError AmsBackendAce::stop_drying(int unit) {
    (void)unit;
    spdlog::info("[ACE] Stopping drying");
    return execute_gcode("ACE_STOP_DRYING");
}

AmsError AmsBackendAce::update_drying(float temp_c, int duration_min, int fan_pct, int unit) {
    auto err = stop_drying(unit);
    if (!err.success()) {
        return err;
    }
    float target_temp = temp_c;
    int target_duration = duration_min;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (temp_c < 0)
            target_temp = dryer_info_.target_temp_c;
        if (duration_min < 0) {
            // A temperature change restarts the cycle, so it must restart on what is
            // left. Reusing the session length would silently hand back time served.
            target_duration = dryer_info_.remaining_min > 0 ? dryer_info_.remaining_min
                                                            : dryer_info_.duration_min;
        }
    }
    return start_drying(target_temp, target_duration, fan_pct, unit);
}

std::vector<DryingPreset> AmsBackendAce::get_drying_presets() const {
    return get_default_drying_presets();
}

// ============================================================================
// Combined ACE Object Parsing (WebSocket subscription path)
// ============================================================================

SlotInfo* AmsBackendAce::mutable_slot_locked(int slot_index) {
    // Caller holds mutex_.
    if (system_info_.units.empty() || slot_index < 0) {
        return nullptr;
    }
    auto& slots = system_info_.units[0].slots;
    if (static_cast<size_t>(slot_index) >= slots.size()) {
        return nullptr;
    }
    return &slots[static_cast<size_t>(slot_index)];
}

void AmsBackendAce::clear_seated_slot_stamp_locked() {
    // Caller holds mutex_.
    if (seated_stamp_slot_ < 0) {
        return;
    }

    SlotInfo* slot = mutable_slot_locked(seated_stamp_slot_);
    // A resized/rebuilt slot vector has already written firmware truth here,
    // so the saved status is stale — only restore over a stamp still visible.
    if (slot != nullptr && slot->status == SlotStatus::LOADED) {
        slot->status = seated_stamp_prev_;
    }

    seated_stamp_slot_ = -1;
    seated_stamp_prev_ = SlotStatus::UNKNOWN;
}

void AmsBackendAce::apply_seated_slot_stamp_locked() {
    // Caller holds mutex_.
    clear_seated_slot_stamp_locked();

    if (!system_info_.filament_loaded || system_info_.current_slot < 0) {
        return;
    }

    SlotInfo* slot = mutable_slot_locked(system_info_.current_slot);
    if (slot == nullptr) {
        return;
    }

    seated_stamp_slot_ = system_info_.current_slot;
    seated_stamp_prev_ = slot->status;
    slot->status = SlotStatus::LOADED;
}

void AmsBackendAce::apply_path_sensors_locked(const json& data) {
    // Caller holds mutex_.
    bool stated = false;
    if (data.contains("rdm_sensor") && data["rdm_sensor"].is_boolean()) {
        rdm_sensor_ = data["rdm_sensor"].get<bool>();
        stated = true;
    }
    if (data.contains("toolhead_sensor") && data["toolhead_sensor"].is_boolean()) {
        toolhead_sensor_ = data["toolhead_sensor"].get<bool>();
        stated = true;
    }
    if (!stated) {
        return;
    }
    path_sensors_seen_ = true;

    // Publish them on the unit so the path canvas knows the hardware is there
    // and can draw the sensor nodes.
    if (!system_info_.units.empty()) {
        auto& unit = system_info_.units[0];
        unit.has_hub_sensor = true;
        unit.hub_sensor_triggered = rdm_sensor_;
        unit.has_toolhead_sensor = true;
    }
}

void AmsBackendAce::apply_dryer_state_locked(const json& data) {
    // Caller holds mutex_. One rule for both producers: the WebSocket object
    // path and the REST /status path parse the same hub, so a dryer spelling
    // one accepts and the other drops is a silent fork in the model.
    //
    // ACE states its temperature top-level (`temp`, the hub's ambient reading
    // near the dryer) — the only reading the Klipper-object spellings carry —
    // so it applies even on a frame with no dryer object. A bridge that ALSO
    // states a current temp inside the dryer object is the more specific
    // reading and wins.
    if (data.contains("temp") && data["temp"].is_number()) {
        dryer_info_.current_temp_c = data["temp"].get<float>();
    }

    const json* dryer = nullptr;
    if (data.contains("dryer") && data["dryer"].is_object()) {
        dryer = &data["dryer"];
    } else if (data.contains("dryer_status") && data["dryer_status"].is_object()) {
        dryer = &data["dryer_status"];
    }

    if (dryer != nullptr) {
        // Klipper-object spelling (ValgACE, native, Kobra S1 fork):
        // {status, target_temp, duration, remain_time}
        if (dryer->contains("status") && (*dryer)["status"].is_string()) {
            const std::string ds = (*dryer)["status"].get<std::string>();
            dryer_info_.active = (ds != "stop" && ds != "idle" && !ds.empty());
        }
        if (dryer->contains("target_temp") && (*dryer)["target_temp"].is_number()) {
            dryer_info_.target_temp_c = (*dryer)["target_temp"].get<float>();
        }
        if (dryer->contains("duration") && (*dryer)["duration"].is_number()) {
            dryer_info_.duration_min = (*dryer)["duration"].get<int>();
        }
        if (dryer->contains("remain_time") && (*dryer)["remain_time"].is_number()) {
            dryer_info_.remaining_min = (*dryer)["remain_time"].get<int>();
        }

        // REST-bridge spelling: {active, current_temp, remaining_minutes,
        // duration_minutes}
        if (dryer->contains("active") && (*dryer)["active"].is_boolean()) {
            dryer_info_.active = (*dryer)["active"].get<bool>();
        }
        if (dryer->contains("current_temp") && (*dryer)["current_temp"].is_number()) {
            dryer_info_.current_temp_c = (*dryer)["current_temp"].get<float>();
        }
        if (dryer->contains("remaining_minutes") &&
            (*dryer)["remaining_minutes"].is_number_integer()) {
            dryer_info_.remaining_min = (*dryer)["remaining_minutes"].get<int>();
        }
        if (dryer->contains("duration_minutes") &&
            (*dryer)["duration_minutes"].is_number_integer()) {
            dryer_info_.duration_min = (*dryer)["duration_minutes"].get<int>();
        }
    }
}

bool AmsBackendAce::seat_from_global_index_locked(int current_index) {
    // Caller holds mutex_.
    if (current_index >= 0) {
        const int slot_count =
            system_info_.units.empty() ? 0 : static_cast<int>(system_info_.units[0].slots.size());
        if (current_index >= slot_count) {
            // Loaded in a unit this backend does not display: state the tool,
            // mark no slot. Idempotent — the REST poll restates the index
            // every cycle.
            const bool changed = system_info_.current_tool != current_index ||
                                 !system_info_.filament_loaded || system_info_.current_slot != -1;
            system_info_.filament_loaded = true;
            system_info_.current_tool = current_index;
            system_info_.current_slot = -1;
            return changed;
        }
    }
    return seat_from_local_index_locked(current_index);
}

bool AmsBackendAce::seat_from_local_index_locked(int slot_index) {
    // Caller holds mutex_.
    const int prev_slot = system_info_.current_slot;
    const int prev_tool = system_info_.current_tool;
    const bool prev_loaded = system_info_.filament_loaded;

    system_info_.filament_loaded = (slot_index >= 0);
    system_info_.current_slot = slot_index;
    system_info_.current_tool = slot_index;

    return system_info_.current_slot != prev_slot || system_info_.current_tool != prev_tool ||
           system_info_.filament_loaded != prev_loaded;
}

void AmsBackendAce::parse_ace_object(const json& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Drop the previous frame's derived seat before the slot loop rewrites
    // statuses, so check_hardware_event_clear and prev_slot_status_ compare
    // firmware against firmware. Re-derived at the bottom of this function.
    clear_seated_slot_stamp_locked();

    // Parse system info (model, firmware)
    if (data.contains("model") && data["model"].is_string()) {
        system_info_.type_name = "ACE";
        // Use model string for unit name if we have units
        auto model = data["model"].get<std::string>();
        if (!system_info_.units.empty()) {
            system_info_.units[0].name = model;
        }
    }

    if (data.contains("firmware") && data["firmware"].is_string()) {
        system_info_.version = data["firmware"].get<std::string>();
    }

    // Parse status string -> AmsAction. The shared vocabulary table is the
    // one AFC and Happy Hare read; ACE's own words (loading/unloading/error,
    // "ready", "drying") resolve to the same actions through it.
    if (data.contains("status") && data["status"].is_string()) {
        system_info_.action = ams_action_from_string(data["status"].get<std::string>());
    }

    // Parse slots array
    if (data.contains("slots") && data["slots"].is_array()) {
        const auto& slots_arr = data["slots"];

        // Sanity check
        if (slots_arr.size() > 16) {
            spdlog::warn("[ACE] Ignoring excessive slot count from get_status: {}",
                         slots_arr.size());
        } else {
            int slot_count = static_cast<int>(slots_arr.size());

            // Ensure we have a unit
            if (system_info_.units.empty()) {
                system_info_.units.emplace_back();
                system_info_.units[0].unit_index = 0;
                system_info_.units[0].connected = true;

                // Use model if available
                if (data.contains("model") && data["model"].is_string()) {
                    system_info_.units[0].name = data["model"].get<std::string>();
                } else {
                    system_info_.units[0].name = "ACE Pro";
                }
            }

            auto& unit = system_info_.units[0];
            unit.slot_count = slot_count;
            system_info_.total_slots = slot_count;

            // Resize slots
            if (unit.slots.size() != static_cast<size_t>(slot_count)) {
                unit.slots.resize(static_cast<size_t>(slot_count));
            }

            for (size_t i = 0; i < slots_arr.size(); ++i) {
                const auto& slot_json = slots_arr[i];
                if (!slot_json.is_object())
                    continue;

                auto& slot = unit.slots[i];
                slot.slot_index = static_cast<int>(i);
                slot.global_index = static_cast<int>(i);

                // Parse status via the shared vocabulary map so the object path
                // and the REST fallback path can't drift.
                //
                // Each parsed value is also kept in a local for the lane
                // observations below, rather than read back off `slot`: the
                // override merge at the bottom of this loop rewrites that
                // member in place and leaves it rewritten for every later
                // frame, so an observation sourced from it would file a user's
                // own edit as the hub's memory.
                const bool frame_states_status =
                    slot_json.contains("status") && slot_json["status"].is_string();
                std::optional<bool> observed_present;
                if (frame_states_status) {
                    const SlotStatus status =
                        slot_status_from_string(slot_json["status"].get<std::string>());
                    observed_present = slot_status_reports_filament(status);
                    slot.status = status;
                }

                // Parse color: ValgACE returns [r, g, b] array
                std::optional<uint32_t> observed_color;
                if (slot_json.contains("color")) {
                    observed_color = parse_slot_color(slot_json["color"]);
                    // A value that carries no colour leaves the bay showing the
                    // last one the hub did state, rather than flipping it to
                    // black - which is a colour ACE really does report.
                    if (observed_color) {
                        slot.color_rgb = *observed_color;
                    }
                }

                // Parse material (e.g., "PLA", "PETG") - see read_slot_material.
                std::optional<std::string> observed_material = read_slot_material(slot_json);
                if (observed_material) {
                    slot.material = *observed_material;
                }

                // Parse SKU if present
                if (slot_json.contains("sku") && slot_json["sku"].is_string()) {
                    // SKU is available but not mapped to SlotInfo currently
                }

                // The rfid field is the bay's tag reader: true means the
                // material and colour in this frame came off the spool's tag;
                // false or absent means the hub is stating its own memory of
                // the bay, which is not a reading of what is in it now
                // (prestonbrown/helixscreen#1710). ValgACE's bridge and the
                // multiACE lineage send the same flag as integers with
                // ACEResearch's PROTOCOL.md states: 0 information not found,
                // 1 failed to identify, 2 identified, 3 identifying. Only 2
                // carries a reading and only 1 is a finished read with no
                // tag. 0 is the reader's idle state, what an empty bay and a
                // just-inserted spool report before the read starts, so like
                // 3 it leaves the evidence incomplete and the insert edge
                // holds its verdict for a later frame.
                helix::ams::SpoolEvidence evidence;
                if (slot_json.contains("rfid")) {
                    const auto& rfid = slot_json["rfid"];
                    if (rfid.is_boolean()) {
                        if (rfid.get<bool>()) {
                            evidence.material = observed_material.value_or(std::string{});
                            evidence.color_rgb = observed_color;
                            evidence.tag_read_complete = true;
                        }
                    } else if (rfid.is_number_integer()) {
                        const auto state = rfid.get<std::int64_t>();
                        if (state == 2) {
                            evidence.material = observed_material.value_or(std::string{});
                            evidence.color_rgb = observed_color;
                            evidence.tag_read_complete = true;
                        } else if (state == 1) {
                            evidence.tag_read_complete = true;
                        }
                    }
                }

                // Insert-edge verdict. Must run BEFORE apply_resolved_lane so
                // the check sees firmware-truth (not the resolved view); once a
                // DifferentSpool clear has run, the lane declares nothing for
                // those fields and apply_resolved_lane leaves them as firmware
                // set them.
                //
                // First observation (no prev_slot_status_ entry) is a
                // BASELINE and must never fire a clear, matching IFS/Snapmaker
                // baseline semantics. Only call the helper when a prior status
                // was already recorded for this slot.
                int idx = static_cast<int>(i);
                auto prev_it = prev_slot_status_.find(idx);
                if (prev_it != prev_slot_status_.end()) {
                    check_hardware_event_clear(slot, idx, prev_it->second, slot.status, evidence);
                }
                prev_slot_status_[idx] = slot.status;
                // Remember the reading for the spool now in the bay, and keep
                // it across the empty interval: the reading of the spool that
                // left is the comparison side of the insert rule when the next
                // one arrives. Only a frame carrying a read writes it: a
                // no-read frame states hub memory, not what the reader got
                // off the occupant, and overwriting with it would lose the
                // comparison side while an insert is still pending. Must
                // follow the check, which reads the previous entry.
                if ((slot.status == SlotStatus::AVAILABLE || slot.status == SlotStatus::LOADED) &&
                    evidence.tag_read_complete) {
                    last_spool_evidence_[idx] = evidence;
                }

                // The hub reports an occupancy status and a colour. Neither is
                // an identity reading, so the colour is the hub's own memory of
                // what was last in the bay rather than proof of what is there.
                //
                // A frame that states no status for this bay files no sensed
                // record: one ingest replaces a source's record whole, so an
                // empty one would erase a live reading instead of repeating it.
                // A status ACE does not recognise DOES file one, with `present`
                // unset - the hub saying it does not know is news, and a record
                // whose field is unset is the only way to retract a reading.
                if (frame_states_status) {
                    helix::ams::Observation sensed(helix::ams::ObservationSource::Sensed);
                    sensed.present = observed_present;
                    helix::ams::ingest(lane_id(idx), sensed);
                }

                // The cache record is filed on every pass, with only what this
                // frame stated in it. `slots` is one Klipper status field, so
                // Moonraker sends the array whole or not at all and a frame
                // that describes a bay describes all of it - there is no
                // partial identity for a whole-record write to narrow. It also
                // makes an absent sensed record provably a guard rather than a
                // translation that did not run.
                helix::ams::Observation cache(helix::ams::ObservationSource::VendorCache);
                cache.color_rgb = observed_color;
                cache.material = observed_material;
                helix::ams::ingest(lane_id(idx), cache);

                // Layer user-configured overrides on top of firmware-reported
                // data. Override wins for any non-default field - for ACE
                // that includes color and material, since ACE hardware
                // doesn't carry brand/spool_name/weights at all and the user
                // edit is the authoritative source for color/material too.
                // The REST poll brings every slot through here each
                // POLL_INTERVAL_MS, so ACE names no slot to cached_slot_locked().
                apply_resolved_lane(slot, idx);
            }
        }
    }

    // Parse dryer state — either outer-key spelling (`dryer` or
    // `dryer_status`), either nested key set, plus the top-level ambient
    // `temp`. See apply_dryer_state_locked.
    apply_dryer_state_locked(data);

    // Populate per-unit environment data for the environment overlay.
    // ACE reports ambient temperature; humidity is not available (left at 0).
    if (!system_info_.units.empty() && dryer_info_.current_temp_c > 0) {
        EnvironmentData env;
        env.temperature_c = dryer_info_.current_temp_c;
        // humidity_pct stays 0 — ACE doesn't have a humidity sensor
        system_info_.units[0].environment = env;
    }

    // Also parse top-level humidity if present (future ValgACE versions)
    if (!system_info_.units.empty() && data.contains("humidity") && data["humidity"].is_number()) {
        if (!system_info_.units[0].environment.has_value()) {
            system_info_.units[0].environment = EnvironmentData{};
        }
        system_info_.units[0].environment->humidity_pct = data["humidity"].get<float>();
        system_info_.units[0].environment->has_humidity = true;
    }

    // Derive loaded slot state from slot statuses
    // ValgACE doesn't have a top-level "loaded_slot" — infer from slot status
    // If any slot is "loaded", that's the active one
    if (!system_info_.units.empty()) {
        for (int i = 0; i < static_cast<int>(system_info_.units[0].slots.size()); ++i) {
            // Check the raw JSON for "loaded" status specifically
            if (data.contains("slots") && data["slots"].is_array() &&
                i < static_cast<int>(data["slots"].size())) {
                const auto& sj = data["slots"][static_cast<size_t>(i)];
                if (sj.contains("status") && sj["status"].is_string() &&
                    sj["status"].get<std::string>() == "loaded") {
                    seat_from_local_index_locked(i);
                    break;
                }
            }
        }
    }

    // Also handle explicit loaded_slot if present (future compatibility)
    if (data.contains("loaded_slot") && data["loaded_slot"].is_number_integer()) {
        seat_from_local_index_locked(data["loaded_slot"].get<int>());
    }

    // Native Anycubic GoKlipper reports the loaded slot as a
    // "current_filament" string of the form "<unitId>-<localIndex>" (e.g.
    // "0-2" = local slot 2). An empty string or absent field means nothing is
    // loaded — in that case leave current_slot as managed by load/unload logic
    // (do NOT force -1 here).
    if (data.contains("current_filament") && data["current_filament"].is_string()) {
        const std::string cf = data["current_filament"].get<std::string>();
        auto dash = cf.find('-');
        if (!cf.empty() && dash != std::string::npos && dash + 1 < cf.size()) {
            try {
                int local_index = std::stoi(cf.substr(dash + 1));
                if (local_index >= 0) {
                    seat_from_local_index_locked(local_index);
                }
            } catch (const std::exception& e) {
                spdlog::debug("[ACE] Failed to parse current_filament '{}': {}", cf, e.what());
            }
        }
    }

    // Kobra S1 fork: the manager object states the seat ONLY as
    // `current_index`, the global tool index, and states it even when
    // nothing is loaded (-1) — so it must clear the seat too, or a TR/unload
    // would leave the previous slot reading loaded forever. Fourth and last
    // explicit signal; last one wins (#1069).
    if (data.contains("current_index") && data["current_index"].is_number_integer()) {
        manager_states_seat_ = true;
        seat_from_global_index_locked(data["current_index"].get<int>());
    }

    apply_path_sensors_locked(data);

    // The master switch. Presence is the capability, so a rig without one is
    // left reporting no bypass rather than one that is permanently off.
    if (data.contains("ace_pro_enabled") && data["ace_pro_enabled"].is_boolean()) {
        ace_pro_enabled_seen_ = true;
        ace_pro_enabled_ = data["ace_pro_enabled"].get<bool>();
        system_info_.supports_bypass = !bypass_on_macro_.empty() && !bypass_off_macro_.empty();
    }

    // All four seated signals (the ValgACE "loaded" scan, loaded_slot, native
    // current_filament, and the fork manager's current_index) have now had
    // their say and arbitrated to one slot; publish that as the slot's own
    // status.
    apply_seated_slot_stamp_locked();
}

const json* AmsBackendAce::select_ace_object(const json& status, std::string* matched_key,
                                             bool require_slots, const json* skip) {
    // One predicate for both selection passes (see the header doc): the
    // subscription-commit pass requires real slot data (a non-empty "slots"
    // array — the exact key parse_ace_object reads — so a manager-only object,
    // the Kobra S1 fork's `ace` with ace_instances/current_index and no slots,
    // falls through to the REST bridge, #1069), while a notify delta needs
    // only a non-empty object the caller has not claimed for itself.
    auto matches = [&](const json& obj) {
        if (!obj.is_object() || obj.empty() || &obj == skip) {
            return false;
        }
        return !require_slots ||
               (obj.contains("slots") && obj["slots"].is_array() && !obj["slots"].empty());
    };

    // Preference order: filament_hub (native GoKlipper), then ace (community
    // ValgACE/BunnyACE), then ace_instance_N (Kobra S1 mainline-Python fork —
    // #1107), so the choice is deterministic.
    if (status.contains("filament_hub") && matches(status["filament_hub"])) {
        if (matched_key)
            *matched_key = "filament_hub";
        return &status["filament_hub"];
    }
    if (status.contains("ace") && matches(status["ace"])) {
        if (matched_key)
            *matched_key = "ace";
        return &status["ace"];
    }
    if (const std::string* instance_key = lowest_ace_instance_key(status, matches)) {
        if (matched_key)
            *matched_key = *instance_key;
        return &status[*instance_key];
    }
    return nullptr;
}

const std::string*
AmsBackendAce::lowest_ace_instance_key(const json& status,
                                       const std::function<bool(const json&)>& predicate) {
    if (!status.is_object()) {
        return nullptr;
    }
    const std::string* best_key = nullptr;
    for (auto it = status.begin(); it != status.end(); ++it) {
        if (it.key().rfind("ace_instance", 0) == 0 && predicate(it.value()) &&
            (best_key == nullptr || it.key() < *best_key)) {
            best_key = &it.key();
        }
    }
    return best_key;
}

const json* AmsBackendAce::manager_ace_object(const json& status) {
    // The fork's manager carries current_index and nothing parseable beyond
    // it; anything else shaped like this (an `ace` with no slots and no
    // current_index) has nothing the seat logic could read, so it is not
    // worth a parse pass.
    if (!status.is_object() || !status.contains("ace") || !status["ace"].is_object()) {
        return nullptr;
    }
    const json& ace = status["ace"];
    if (ace.contains("slots") && ace["slots"].is_array() && !ace["slots"].empty()) {
        return nullptr; // slot-bearing ValgACE `ace` — the primary, not a manager
    }
    if (!ace.contains("current_index") || !ace["current_index"].is_number_integer()) {
        return nullptr;
    }
    return &ace;
}

SlotStatus AmsBackendAce::slot_status_from_string(const std::string& status_str) {
    // Native Anycubic GoKlipper uses empty/ready/preload/running/runout;
    // community ValgACE uses available/loaded/ready. "runout" means the slot
    // ran dry mid-print — present-but-empty; map to EMPTY since SlotStatus has
    // no dedicated RUNOUT value. Anything unrecognized (incl. "unknown") maps
    // to UNKNOWN.
    if (status_str == "empty" || status_str == "runout") {
        return SlotStatus::EMPTY;
    }
    if (status_str == "available" || status_str == "loaded" || status_str == "ready" ||
        status_str == "preload" || status_str == "running") {
        return SlotStatus::AVAILABLE;
    }
    return SlotStatus::UNKNOWN;
}

std::optional<std::string> AmsBackendAce::read_slot_material(const json& slot_json) {
    auto get_str = [&slot_json](const char* key) -> std::optional<std::string> {
        if (slot_json.contains(key) && slot_json[key].is_string()) {
            auto value = slot_json[key].get<std::string>();
            if (!value.empty()) {
                return value;
            }
        }
        return std::nullopt;
    };
    if (auto material = get_str("material")) {
        return material;
    }
    return get_str("type");
}

std::optional<uint32_t> AmsBackendAce::parse_slot_color(const json& color_val) {
    // ValgACE format: [r, g, b] array. A triplet is the hub's own numeric
    // spelling, not a colour string, so the shared text grammar has nothing to
    // say about it.
    if (color_val.is_array() && color_val.size() >= 3) {
        try {
            uint8_t r = static_cast<uint8_t>(color_val[0].get<int>());
            uint8_t g = static_cast<uint8_t>(color_val[1].get<int>());
            uint8_t b = static_cast<uint8_t>(color_val[2].get<int>());
            return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
                   static_cast<uint32_t>(b);
        } catch (const std::exception& e) {
            spdlog::debug("[ACE] Failed to parse color array: {}", e.what());
            return std::nullopt;
        }
    }

    // REST bridge format: hex string "#RRGGBB" or "0xRRGGBB", read by the same
    // grammar as every other lane-shaped producer. Both of the helper's
    // non-colour answers are "no reading" here: the bridge states an absent
    // colour by omitting the key, so an empty string is a malformed value
    // rather than a bay the hub is clearing.
    if (color_val.is_string()) {
        const auto reading = helix::ams::read_lane_color(color_val.get<std::string>());
        if (reading.kind == helix::ams::ColorReadingKind::Observed) {
            return reading.rgb;
        }
        spdlog::debug("[ACE] Slot color '{}' carries no colour", color_val.get<std::string>());
    }

    return std::nullopt;
}

// ============================================================================
// REST Fallback (for BunnyACE/DuckACE)
// ============================================================================

void AmsBackendAce::start_rest_fallback() {
    use_rest_fallback_ = true;
    rest_stop_requested_.store(false);
    // Reap any prior thread before move-assignment. Reassigning to a joinable
    // std::thread calls std::terminate WITHOUT an active exception, regardless
    // of whether the loop has already exited (UBZQ94EE-class bug).
    if (rest_polling_thread_.joinable()) {
        rest_polling_thread_.join();
    }
    // Wrap — EAGAIN under thread exhaustion throws std::system_error ([L083]).
    try {
        rest_polling_thread_ = std::thread(&AmsBackendAce::rest_polling_loop, this);
        spdlog::info("[ACE] REST fallback polling started");
    } catch (const std::system_error& e) {
        spdlog::error("[ACE] Failed to spawn REST polling thread: {}", e.what());
        use_rest_fallback_ = false;
    }
}

void AmsBackendAce::stop_rest_fallback() {
    if (!use_rest_fallback_)
        return;

    rest_stop_requested_.store(true);
    {
        std::lock_guard<std::mutex> lock(rest_stop_mutex_);
        rest_stop_cv_.notify_all();
    }
    if (rest_polling_thread_.joinable()) {
        rest_polling_thread_.join();
    }
    use_rest_fallback_ = false;
    spdlog::debug("[ACE] REST fallback polling stopped");
}

void AmsBackendAce::rest_polling_loop() {
    spdlog::debug("[ACE] REST polling thread started");

    // Fetch system info first
    poll_info();

    while (!rest_stop_requested_.load()) {
        // /server/ace/info is OPTIONAL — best-effort retry until the cap for the
        // model/version it can supply, but its absence is NOT fatal: the Kobra
        // S1 fork ships /status + /slots (which carry model + slots) but NOT
        // /info (#1069). Give up quietly after the cap.
        if (!info_fetched_.load() && info_fetch_failures_.load() < MAX_INFO_FETCH_FAILURES) {
            poll_info();
        } else if (!info_fetched_.load() &&
                   info_fetch_failures_.load() == MAX_INFO_FETCH_FAILURES) {
            spdlog::info("[ACE] /server/ace/info unavailable after {} attempts — "
                         "using /status + /slots (this fork has no /info endpoint).",
                         MAX_INFO_FETCH_FAILURES);
            ++info_fetch_failures_;
        }

        // Data endpoints are the source of truth for slots (and model, via the
        // /status envelope) — poll them unconditionally, NOT gated on
        // info_fetched_. Gating here left the fork's backend permanently idle.
        poll_status();
        poll_slots();

        // Interruptible sleep
        std::unique_lock<std::mutex> lock(rest_stop_mutex_);
        rest_stop_cv_.wait_for(lock, std::chrono::milliseconds(POLL_INTERVAL_MS),
                               [this] { return rest_stop_requested_.load(); });
    }

    spdlog::debug("[ACE] REST polling thread exiting");
}

void AmsBackendAce::poll_info() {
    if (!api_) {
        return;
    }

    spdlog::debug("[ACE] Polling /server/ace/info");

    struct SyncState {
        std::mutex mtx;
        std::condition_variable cv;
        bool done{false};
    };
    auto state = std::make_shared<SyncState>();

    auto token = lifetime_.token();

    api_->rest().call_rest_get("/server/ace/info", [this, state, token](const RestResponse& resp) {
        // L081 Mechanism C: defer member access to main thread. The synchronous
        // waiter (state->cv) MUST be signaled regardless of owner liveness, so
        // we enqueue the parse defer first, then signal state->done + cv on the
        // bg thread before returning. The defer becomes a no-op if the owner
        // has been destroyed; the poll_info caller is unblocked either way.
        token.defer("AmsBackendAce::poll_info_apply", [this, resp]() {
            if (resp.success && resp.data.contains("result")) {
                parse_info_response(resp.data["result"]);
                info_fetched_.store(true);
                info_fetch_failures_ = 0;
            } else {
                // /info is optional (#1069) — a failure here is NOT surfaced to
                // the user. The "bridge not found" error is gated on the DATA
                // endpoints (/status + /slots) failing instead; see poll_status.
                int failures = ++info_fetch_failures_;
                spdlog::debug("[ACE] /server/ace/info attempt {} failed (optional): {}", failures,
                              resp.error);
            }
        });

        {
            std::lock_guard<std::mutex> lock(state->mtx);
            state->done = true;
        }
        state->cv.notify_one();
    });

    // Wait for response (with timeout)
    std::unique_lock<std::mutex> lock(state->mtx);
    state->cv.wait_for(lock, std::chrono::seconds(5), [state] { return state->done; });
}

void AmsBackendAce::poll_status() {
    if (!api_) {
        return;
    }

    spdlog::trace("[ACE] Polling /server/ace/status");

    auto token = lifetime_.token();

    api_->rest().call_rest_get("/server/ace/status", [this, token](const RestResponse& resp) {
        // L081 Mechanism C: defer member access (parse_status_response,
        // emit_event) to main thread.
        token.defer("AmsBackendAce::poll_status_apply", [this, resp]() {
            if (resp.success && resp.data.contains("result")) {
                rest_data_ok_.store(true);
                data_fetch_failures_.store(0);
                if (parse_status_response(resp.data["result"])) {
                    emit_event(EVENT_STATE_CHANGED);
                }
            } else {
                int failures = ++data_fetch_failures_;
                spdlog::debug("[ACE] Status poll attempt {} failed: {}", failures, resp.error);
                // Genuinely-missing-bridge case: the DATA endpoints are down and
                // nothing has ever succeeded. Surface it once. A working /slots
                // resets data_fetch_failures_, so this only fires when BOTH
                // /status and /slots are unavailable (#1069).
                if (failures == MAX_DATA_FETCH_FAILURES && !rest_data_ok_.load()) {
                    spdlog::warn("[ACE] Moonraker ACE bridge not responding at /server/ace/status "
                                 "after {} attempts — no ACE data endpoints available.",
                                 failures);
                    emit_event(EVENT_ERROR,
                               "ACE detected but Moonraker bridge not found. "
                               "Install the ace_status.py component for full ACE support.");
                    helix::ui::queue_update([]() {
                        ToastManager::instance().show(
                            ToastSeverity::WARNING,
                            lv_tr("ACE Moonraker bridge not found. Install ace_status.py "
                                  "for full support."),
                            6000);
                    });
                }
            }
        });
    });
}

void AmsBackendAce::poll_slots() {
    if (!api_) {
        return;
    }

    spdlog::trace("[ACE] Polling /server/ace/slots");

    auto token = lifetime_.token();

    api_->rest().call_rest_get("/server/ace/slots", [this, token](const RestResponse& resp) {
        // L081 Mechanism C: defer member access (parse_slots_response,
        // emit_event) to main thread.
        token.defer("AmsBackendAce::poll_slots_apply", [this, resp]() {
            if (resp.success && resp.data.contains("result")) {
                // A working /slots is sufficient data — latch data-ok and reset
                // the failure counter so the "bridge not found" error only fires
                // when BOTH data endpoints are down (#1069).
                rest_data_ok_.store(true);
                data_fetch_failures_.store(0);
                if (parse_slots_response(resp.data["result"])) {
                    // REST fallback parses all slots at once — use STATE_CHANGED
                    // (full-sync semantics) rather than SLOT_CHANGED without a
                    // slot_index.
                    emit_event(EVENT_STATE_CHANGED);
                }
            } else {
                spdlog::debug("[ACE] Slots poll failed: {}", resp.error);
            }
        });
    });
}

// ============================================================================
// REST Response Parsing (fallback path)
// ============================================================================

void AmsBackendAce::parse_info_response(const json& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (data.contains("model") && data["model"].is_string()) {
        system_info_.type_name = "ACE";
    }

    if (data.contains("version") && data["version"].is_string()) {
        system_info_.version = data["version"].get<std::string>();
    }

    if (data.contains("slot_count") && data["slot_count"].is_number_integer()) {
        int slot_count = data["slot_count"].get<int>();

        if (slot_count < 0 || slot_count > 16) {
            spdlog::warn("[ACE] Ignoring invalid slot_count: {}", slot_count);
            return;
        }

        system_info_.total_slots = slot_count;

        if (system_info_.units.empty()) {
            system_info_.units.emplace_back();
            system_info_.units[0].name = "ACE Pro";
            system_info_.units[0].unit_index = 0;
            system_info_.units[0].connected = true;
        }

        auto& unit = system_info_.units[0];
        unit.slot_count = slot_count;

        if (unit.slots.size() != static_cast<size_t>(slot_count)) {
            unit.slots.resize(static_cast<size_t>(slot_count));
            for (int i = 0; i < slot_count; ++i) {
                unit.slots[static_cast<size_t>(i)].slot_index = i;
                unit.slots[static_cast<size_t>(i)].global_index = i;
                unit.slots[static_cast<size_t>(i)].status = SlotStatus::UNKNOWN;
            }
        }
    }

    spdlog::info("[ACE] Detected: {} v{} with {} slots", system_info_.type_name,
                 system_info_.version, system_info_.total_slots);
}

bool AmsBackendAce::parse_status_response(const json& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = false;

    // The fork's /server/ace/status envelope carries model + firmware, so the
    // backend can identify the hardware without /server/ace/info (#1069). Match
    // the object-path key names (`model`, `firmware`).
    if (data.contains("model") && data["model"].is_string()) {
        system_info_.type_name = "ACE";
        auto model = data["model"].get<std::string>();
        if (!system_info_.units.empty()) {
            system_info_.units[0].name = model;
        }
    }
    if (data.contains("firmware") && data["firmware"].is_string()) {
        system_info_.version = data["firmware"].get<std::string>();
    }

    if (data.contains("loaded_slot") && data["loaded_slot"].is_number_integer()) {
        changed |= seat_from_local_index_locked(data["loaded_slot"].get<int>());
    }

    // The fork's /status states the seat through the manager's
    // current_index instead of loaded_slot — same semantics, global tool
    // index with -1 meaning nothing loaded (#1069).
    if (data.contains("ace_manager") && data["ace_manager"].is_object() &&
        data["ace_manager"].contains("current_index") &&
        data["ace_manager"]["current_index"].is_number_integer()) {
        manager_states_seat_ = true;
        apply_path_sensors_locked(data["ace_manager"]);
        changed |= seat_from_global_index_locked(data["ace_manager"]["current_index"].get<int>());
    }

    // The ValgACE bridge names the action field `action`; the fork names it
    // `status` ("ready"/etc.) with the same loading/unloading/error
    // vocabulary the object path maps (#1069).
    std::string action_str;
    if (data.contains("action") && data["action"].is_string()) {
        action_str = data["action"].get<std::string>();
    } else if (data.contains("status") && data["status"].is_string()) {
        action_str = data["status"].get<std::string>();
    }
    // The ValgACE bridge names the action field `action`; the fork names it
    // `status` ("ready"/etc.) with the same loading/unloading/error
    // vocabulary the object path maps (#1069). A bridge `action` is
    // authoritative; the fork's fallback reads the UNIT's status, which can
    // stay "ready" through a toolchange (do_load_filament sets LOADING
    // optimistically for exactly that case), so an IDLE resolved from it
    // must not demote an in-flight local load/unload.
    const bool action_from_unit_status = !data.contains("action");
    if (!action_str.empty()) {
        const AmsAction action = ams_action_from_string(action_str);
        const bool demotes_in_flight_op = action_from_unit_status && action == AmsAction::IDLE &&
                                          (system_info_.action == AmsAction::LOADING ||
                                           system_info_.action == AmsAction::UNLOADING);
        if (!demotes_in_flight_op && action != system_info_.action) {
            system_info_.action = action;
            changed = true;
        }
    }

    apply_dryer_state_locked(data);

    // /status owns loaded_slot but never touches the slot vector; /slots owns
    // the slot vector but carries no seated field. Both ends re-derive the
    // stamp so whichever polled last leaves the two consistent.
    apply_seated_slot_stamp_locked();

    return changed;
}

bool AmsBackendAce::parse_slots_response(const json& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = false;

    if (!data.contains("slots") || !data["slots"].is_array()) {
        return false;
    }

    const auto& slots_data = data["slots"];

    if (slots_data.size() > 16) {
        spdlog::warn("[ACE] Ignoring excessive slot count: {}", slots_data.size());
        return false;
    }

    if (system_info_.units.empty()) {
        system_info_.units.emplace_back();
        system_info_.units[0].name = "ACE Pro";
        system_info_.units[0].unit_index = 0;
        system_info_.units[0].connected = true;
    }

    auto& unit = system_info_.units[0];

    if (unit.slots.size() != slots_data.size()) {
        unit.slots.resize(slots_data.size());
        unit.slot_count = static_cast<int>(slots_data.size());
        system_info_.total_slots = static_cast<int>(slots_data.size());
        changed = true;
    }

    // Un-stamp before the per-slot compare below, or a seated slot would read
    // as changed against firmware's "ready" on every 500 ms poll and emit a
    // STATE_CHANGED event forever.
    clear_seated_slot_stamp_locked();

    for (size_t i = 0; i < slots_data.size(); ++i) {
        const auto& slot_json = slots_data[i];

        if (!slot_json.is_object()) {
            continue;
        }

        auto& slot = unit.slots[i];

        slot.slot_index = static_cast<int>(i);
        slot.global_index = static_cast<int>(i);

        const bool states_status = slot_json.contains("status") && slot_json["status"].is_string();
        std::optional<bool> observed_present;
        if (states_status) {
            // Mirror the object path exactly via the shared vocabulary map —
            // this fork emits `ready` (and can emit `unknown`), which the old
            // empty/available/loaded-only mapping misclassified (#1069).
            SlotStatus status = slot_status_from_string(slot_json["status"].get<std::string>());
            observed_present = slot_status_reports_filament(status);

            if (status != slot.status) {
                slot.status = status;
                changed = true;
            }
        }

        // Parse color: handle both hex string and RGB array formats
        std::optional<uint32_t> observed_color;
        if (slot_json.contains("color")) {
            observed_color = parse_slot_color(slot_json["color"]);
            if (observed_color && *observed_color != slot.color_rgb) {
                slot.color_rgb = *observed_color;
                changed = true;
            }
        }

        // Material — see read_slot_material (one reader for both producers).
        const std::optional<std::string> material = read_slot_material(slot_json);
        if (material && *material != slot.material) {
            slot.material = *material;
            changed = true;
        }

        // The bridge polls the same hub the subscription reads, so it files the
        // same account on the same lanes. Its values come from this response's
        // own keys rather than from `slot`, which a previous poll's diff has
        // already written - and which, on a rig that once ran the object path,
        // apply_resolved_lane has laid a user's declaration onto.
        const int idx = static_cast<int>(i);
        if (states_status) {
            helix::ams::Observation sensed(helix::ams::ObservationSource::Sensed);
            sensed.present = observed_present;
            helix::ams::ingest(lane_id(idx), sensed);
        }

        helix::ams::Observation cache(helix::ams::ObservationSource::VendorCache);
        cache.color_rgb = observed_color;
        cache.material = material;
        helix::ams::ingest(lane_id(idx), cache);

        if (slot_json.contains("temp_min") && slot_json["temp_min"].is_number_integer()) {
            slot.nozzle_temp_min = slot_json["temp_min"].get<int>();
        }
        if (slot_json.contains("temp_max") && slot_json["temp_max"].is_number_integer()) {
            slot.nozzle_temp_max = slot_json["temp_max"].get<int>();
        }
    }

    // /slots carries no seated field, so re-apply what /status last resolved —
    // otherwise this poll would silently demote the loaded slot to AVAILABLE
    // and take can_unload_from_toolhead() with it.
    apply_seated_slot_stamp_locked();

    return changed;
}

// ============================================================================
// Device Actions
// ============================================================================

std::vector<helix::printer::DeviceSection> AmsBackendAce::get_device_sections() const {
    using DS = helix::printer::DeviceSection;
    return {
        DS{"filament_control", "Filament Control", 0, "Manual feed and retract operations"},
        DS{"maintenance", "Maintenance", 1, "Feed assist and debug tools"},
    };
}

std::vector<helix::printer::DeviceAction> AmsBackendAce::get_device_actions() const {
    using DA = helix::printer::DeviceAction;
    using AT = helix::printer::ActionType;
    return {
        DA{"ace_manual_feed",
           "Manual Feed",
           "",
           "filament_control",
           "Feed filament from current slot",
           AT::BUTTON,
           {},
           {},
           0,
           100,
           "",
           -1,
           true,
           ""},
        DA{"ace_manual_retract",
           "Manual Retract",
           "",
           "filament_control",
           "Retract filament to current slot",
           AT::BUTTON,
           {},
           {},
           0,
           100,
           "",
           -1,
           true,
           ""},
        DA{"ace_feed_assist_toggle",
           "Feed Assist",
           "",
           "maintenance",
           "Enable feed assist for active slot during printing",
           AT::TOGGLE,
           {},
           {},
           0,
           100,
           "",
           -1,
           true,
           ""},
    };
}

AmsError AmsBackendAce::execute_device_action(const std::string& action_id, const std::any& value) {
    if (action_id == "ace_manual_feed") {
        int slot = get_current_slot();
        if (slot < 0)
            slot = 0;
        static constexpr int MANUAL_FEED_LENGTH = 50;
        static constexpr int MANUAL_FEED_SPEED = 50;
        return execute_gcode("ACE_FEED INDEX=" + std::to_string(slot) +
                             " LENGTH=" + std::to_string(MANUAL_FEED_LENGTH) +
                             " SPEED=" + std::to_string(MANUAL_FEED_SPEED));
    }

    if (action_id == "ace_manual_retract") {
        int slot = get_current_slot();
        if (slot < 0)
            slot = 0;
        static constexpr int MANUAL_RETRACT_LENGTH = 50;
        static constexpr int MANUAL_RETRACT_SPEED = 50;
        return execute_gcode("ACE_RETRACT INDEX=" + std::to_string(slot) +
                             " LENGTH=" + std::to_string(MANUAL_RETRACT_LENGTH) +
                             " SPEED=" + std::to_string(MANUAL_RETRACT_SPEED));
    }

    if (action_id == "ace_feed_assist_toggle") {
        int slot = get_current_slot();
        if (slot < 0)
            slot = 0;

        bool enable = true;
        if (value.has_value()) {
            try {
                enable = std::any_cast<bool>(value);
            } catch (const std::bad_any_cast&) {
                // Default to enable if cast fails
            }
        }

        if (enable) {
            return execute_gcode("ACE_ENABLE_FEED_ASSIST INDEX=" + std::to_string(slot));
        } else {
            return execute_gcode("ACE_DISABLE_FEED_ASSIST INDEX=" + std::to_string(slot));
        }
    }

    return AmsErrorHelper::not_supported("Unknown ACE action: " + action_id);
}

// ============================================================================
// Slot Override Layering (shared FilamentSlotOverrideStore)
// ============================================================================

void AmsBackendAce::check_hardware_event_clear(SlotInfo& slot, int slot_index, SlotStatus prev,
                                               SlotStatus curr,
                                               const helix::ams::SpoolEvidence& inserted) {
    // "A spool was put in" is the status transition EMPTY -> present
    // (AVAILABLE / LOADED); what that insert does to the record is the insert
    // rule's verdict on the two tag readings. A LOADED -> EMPTY transition is
    // not an insert, and UNKNOWN is "no signal" on either side; callers must
    // only invoke this helper AFTER a valid prior observation has been
    // recorded (caller handles the baseline skip).
    const bool was_empty = (prev == SlotStatus::EMPTY);
    const bool is_present = (curr == SlotStatus::AVAILABLE || curr == SlotStatus::LOADED);
    if (!is_present) {
        // A bay that empties with an insert still waiting on its tag read
        // drops the insert: the spool left before the reader ever stated
        // one, and there is nothing in the bay to ask about.
        pending_insert_reads_.erase(slot_index);
        return;
    }
    if (!was_empty) {
        // Not an edge. The one thing a non-edge frame still owes is a
        // pending insert: the bay reported the spool on an earlier frame and
        // this one either carries the read it was waiting for, or is another
        // pass with none.
        auto pit = pending_insert_reads_.find(slot_index);
        if (pit == pending_insert_reads_.end())
            return;
        if (inserted.tag_read_complete) {
            judge_insert_locked(slot, slot_index, inserted);
            pending_insert_reads_.erase(pit);
        } else if (++pit->second >= kAcePendingReadParsePasses) {
            // The read never landed. An empty evidence is NoEvidence under
            // the rule, which offers the notice rather than clearing.
            judge_insert_locked(slot, slot_index, helix::ams::SpoolEvidence{});
            pending_insert_reads_.erase(pit);
        }
        return;
    }

    // EMPTY -> present, the insert edge. A frame carrying no read arms
    // rather than judges: the read can land frames later, and judging on
    // nothing would file every insert as no-evidence.
    if (!inserted.tag_read_complete) {
        spdlog::debug("[ACE] Slot {} inserted; tag read not landed yet, holding the verdict",
                      slot_index);
        pending_insert_reads_[slot_index] = 0;
        return;
    }
    judge_insert_locked(slot, slot_index, inserted);
}

void AmsBackendAce::judge_insert_locked(SlotInfo& slot, int slot_index,
                                        const helix::ams::SpoolEvidence& inserted) {
    std::optional<helix::ams::SpoolEvidence> before;
    if (const auto prev_reading = last_spool_evidence_.find(slot_index);
        prev_reading != last_spool_evidence_.end()) {
        before = prev_reading->second;
    }
    const auto verdict = helix::ams::classify_insert(before, inserted);

    if (verdict == helix::ams::InsertVerdict::NoEvidence) {
        // An insert the hardware read nothing about: the bay said no rfid, or
        // the previous occupant left no reading. Nothing contradicts the
        // record and nothing confirms it either, so ask rather than clear.
        // offer_clear_after_unverified_insert() re-checks every guard
        // (print-feeding lane, lane with nothing to clear) on the UI thread.
        spdlog::debug("[ACE] Slot {} inserted with no comparable tag reading; "
                      "offering the same-spool notice",
                      slot_index);
        helix::ui::queue_update(
            [slot_index]() { helix::ui::offer_clear_after_unverified_insert(slot_index); });
        return;
    }
    if (verdict != helix::ams::InsertVerdict::DifferentSpool) {
        spdlog::debug("[ACE] Slot {} inserted with the same tag reading; record stands",
                      slot_index);
        return;
    }

    auto ovr_it = overrides_.find(slot_index);
    if (ovr_it == overrides_.end()) {
        spdlog::debug("[ACE] Slot {} inserted with a different tag reading; "
                      "no override to clear",
                      slot_index);
        return;
    }

    spdlog::info("[ACE] Slot {} inserted with a different tag reading; clearing override",
                 slot_index);

    // Delegate erase + field reset + clear_async to the shared helper so
    // hardware-event clears and user-initiated clears share one field-reset
    // policy. Caller already holds mutex_.
    (void)ovr_it;
    clear_override_locked(slot_index, slot);
}

void AmsBackendAce::clear_override_locked(int slot_index, SlotInfo& slot) {
    // Caller must hold mutex_. Erases the in-memory override, resets
    // override-exclusive fields on the live SlotInfo so the cleared state
    // is visible in the very next get_slot_info() read. ACE field policy:
    // brand / spool_name / spoolman_* / weights / color_name are override-only
    // (firmware doesn't populate them). Color and material come from the
    // parse and are left alone so the new spool's firmware data surfaces.
    overrides_.erase(slot_index);
    // The lane's own records go with it: the erase above and this are one
    // clear in two stores, and a clear that reached only one would leave
    // resolve() still reporting the identity just removed.
    helix::ams::reset_lane_to_machine_readings(lane_id(slot_index));

    slot.brand.clear();
    slot.clear_spoolman_link();
    slot.remaining_weight_g = -1.0f;
    slot.total_weight_g = -1.0f;
    slot.color_name.clear();
    // The catalog pick is override-exclusive on every backend — no AMS
    // firmware carries a branded product id — so a clear always drops it.
    // Leaving it would re-navigate the editor to the removed spool's
    // product on the next open.
    slot.catalog_id.clear();
    slot.product_name.clear();

    if (override_store_) {
        // Capture by value — clear_async's Moonraker callback can fire after
        // this returns (MR tracker ~60s) and potentially after the backend
        // itself is gone. Same rationale as save_async.
        const std::string tag = backend_log_tag();
        override_store_->clear_async(slot_index, [tag, slot_index](bool ok, std::string err) {
            if (!ok) {
                spdlog::warn("{} clear_async failed for slot {}: {}", tag, slot_index, err);
            }
        });
    }
}

void AmsBackendAce::clear_slot_override(int slot_index) {
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

} // namespace helix

#endif // HELIX_HAS_ACE
