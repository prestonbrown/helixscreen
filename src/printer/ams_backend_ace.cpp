// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_backend_ace.cpp
 * @brief ACE (AnyCubic ACE Pro) backend implementation
 *
 * Primary path: WebSocket subscription to ace Klipper object (ValgACE).
 * Fallback path: REST polling via /server/ace/ endpoints (BunnyACE/DuckACE).
 * See ams_backend_ace.h for full documentation.
 */

#include "ams_backend_ace.h"

#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "post_op_cooldown_manager.h"
#include "settings_manager.h"
#include "spdlog/spdlog.h"

#include <spdlog/fmt/fmt.h>

#include <chrono>

using json = nlohmann::json;
using namespace helix;

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
        override_store_ = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
            api_, "ace", helix::ams::lane_key_style_for(get_type()));
        // Do the (potentially 5s) MR DB round-trip OUTSIDE the lock, then swap
        // in under mutex_. Holding mutex_ during the swap ensures the parse
        // path sees a coherent map rather than a torn write.
        auto loaded = override_store_->load_blocking();
        const auto loaded_count = loaded.size();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            overrides_ = std::move(loaded);
        }
        spdlog::info("[ACE] Loaded {} slot overrides from filament_slot store", loaded_count);
    }

    auto token = lifetime_.token();

    // Query all known Klipper object names directly (works if driver has
    // get_status()). Native Anycubic GoKlipper registers the object as
    // `filament_hub`; community ValgACE/BunnyACE/DuckACE register it as `ace`;
    // the Kobra S1 mainline-Python fork registers each unit as
    // `ace_instance_N` (#1107). printer.objects.query tolerates unknown
    // objects (absent from the result) and select_slot_bearing_object handles
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
                    ace_data =
                        select_slot_bearing_object(response["result"]["status"], &matched_key);
                }

                if (ace_data) {
                    // Native/ValgACE path: got real data from get_status()
                    spdlog::info("[ACE] Klipper '{}' object has status data — "
                                 "using native WebSocket subscription",
                                 matched_key);

                    parse_ace_object(*ace_data);
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
    // `ace_instance_N` (#1107). Preference order: filament_hub, ace, then the
    // lowest-numbered ace_instance_N.
    const json* ace_data = nullptr;
    if (status->contains("filament_hub") && (*status)["filament_hub"].is_object() &&
        !(*status)["filament_hub"].empty()) {
        ace_data = &(*status)["filament_hub"];
    } else if (status->contains("ace") && (*status)["ace"].is_object() &&
               !(*status)["ace"].empty()) {
        ace_data = &(*status)["ace"];
    } else {
        const std::string* best_key = nullptr;
        for (auto it = status->begin(); it != status->end(); ++it) {
            if (it.key().rfind("ace_instance", 0) == 0 && it.value().is_object() &&
                !it.value().empty()) {
                if (best_key == nullptr || it.key() < *best_key) {
                    best_key = &it.key();
                }
            }
        }
        if (best_key) {
            ace_data = &(*status)[*best_key];
        }
    }
    if (!ace_data)
        return;

    parse_ace_object(*ace_data);
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

    if (!system_info_.filament_loaded) {
        return PathSegment::NONE;
    }
    return PathSegment::NOZZLE;
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

    spdlog::info("[ACE] Executing G-code: {}", gcode);
    api_->execute_gcode(
        gcode,
        [this, token, slot_index]() {
            // L081 Mechanism C: marshal member writes (system_info_) to main.
            token.defer("AmsBackendAce::load_done", [this, slot_index]() {
                spdlog::info("[ACE] Load slot {} gcode completed", slot_index);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    system_info_.action = AmsAction::IDLE;
                    system_info_.current_slot = slot_index;
                    system_info_.current_tool = slot_index;
                    system_info_.filament_loaded = true;

                    // Same derivation the parse paths use, so the next status
                    // frame re-applies this stamp instead of erasing it.
                    apply_seated_slot_stamp_locked();
                }
                PostOpCooldownManager::instance().schedule();
                emit_event(EVENT_STATE_CHANGED);
            });
        },
        [this, token, gcode](const MoonrakerError& err) {
            // Log + reset to IDLE only — nothing here reaches the user, so the
            // execute_gcode() call below declares caller_surfaces_errors=false.
            token.defer("AmsBackendAce::load_err", [this, err, gcode]() {
                if (err.type == MoonrakerErrorType::TIMEOUT) {
                    spdlog::warn("[ACE] Load gcode timed out (may still be running): {}", gcode);
                } else {
                    spdlog::error("[ACE] Load gcode failed: {} - {}", gcode, err.message);
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    system_info_.action = AmsAction::IDLE;
                }
                emit_event(EVENT_STATE_CHANGED);
            });
        },
        IMoonrakerAPI::AMS_OPERATION_TIMEOUT_MS, /*silent=*/false, /*on_queued=*/nullptr,
        // See include/rpc_error_policy.h: a log-only error callback that claims
        // the report silences GcodeErrorRouter's `!!` copy, which is the only
        // surface that would have told the user the load failed.
        /*caller_surfaces_errors=*/false);

    return AmsErrorHelper::success();
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

    spdlog::info("[ACE] Executing G-code: {}", gcode);
    api_->execute_gcode(
        gcode,
        [this, token]() {
            // L081 Mechanism C: marshal member writes (system_info_) to main.
            token.defer("AmsBackendAce::unload_done", [this]() {
                spdlog::info("[ACE] Unload gcode completed");
                {
                    std::lock_guard<std::mutex> lock(mutex_);

                    system_info_.action = AmsAction::IDLE;
                    system_info_.current_slot = -1;
                    system_info_.current_tool = -1;
                    system_info_.filament_loaded = false;

                    // Releases the stamp back to the status the parse wrote,
                    // rather than assuming AVAILABLE for a slot firmware may
                    // have called EMPTY.
                    apply_seated_slot_stamp_locked();
                }
                PostOpCooldownManager::instance().schedule();
                emit_event(EVENT_STATE_CHANGED);
            });
        },
        [this, token, gcode](const MoonrakerError& err) {
            // Log + reset to IDLE only — nothing here reaches the user, so the
            // execute_gcode() call below declares caller_surfaces_errors=false.
            token.defer("AmsBackendAce::unload_err", [this, err, gcode]() {
                if (err.type == MoonrakerErrorType::TIMEOUT) {
                    spdlog::warn("[ACE] Unload gcode timed out (may still be running): {}", gcode);
                } else {
                    spdlog::error("[ACE] Unload gcode failed: {} - {}", gcode, err.message);
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    system_info_.action = AmsAction::IDLE;
                }
                emit_event(EVENT_STATE_CHANGED);
            });
        },
        IMoonrakerAPI::AMS_OPERATION_TIMEOUT_MS, /*silent=*/false, /*on_queued=*/nullptr,
        // Same reasoning as the load path above — see include/rpc_error_policy.h.
        /*caller_surfaces_errors=*/false);

    return AmsErrorHelper::success();
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

AmsError AmsBackendAce::set_slot_info(int slot_index, const SlotInfo& info, bool persist) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Validate slot index
        if (system_info_.units.empty() || slot_index < 0 ||
            slot_index >= static_cast<int>(system_info_.units[0].slots.size())) {
            return AmsErrorHelper::invalid_slot(slot_index, 0);
        }

        // Update in-memory slot state so get_slot_info returns the edit
        // immediately — covers every SlotInfo field the caller may have set,
        // including persist=false previews that must survive until the next
        // firmware parse.
        auto& slot = system_info_.units[0].slots[slot_index];
        slot.color_rgb = info.color_rgb;
        slot.color_name = info.color_name;
        slot.material = info.material;
        slot.brand = info.brand;
        // Carry the catalog product identity through preview writes too — a
        // persist=false preview that dropped it would make the editor snap
        // back to a different variant on the next get_slot_info().
        slot.catalog_id = info.catalog_id;
        slot.product_name = info.product_name;
        slot.spool_name = info.spool_name;
        slot.spoolman_id = info.spoolman_id;
        slot.spoolman_vendor_id = info.spoolman_vendor_id;
        slot.remaining_weight_g = info.remaining_weight_g;
        slot.total_weight_g = info.total_weight_g;

        // For persist=true, stage the override into overrides_ so
        // apply_overrides re-applies the new values on every subsequent parse.
        // For persist=false we explicitly do NOT touch overrides_ — preview
        // edits are in-memory only and will be overwritten by the next
        // firmware parse (expected preview contract).
        if (persist) {
            helix::ams::FilamentSlotOverride ovr;
            ovr.brand = info.brand;
            ovr.spool_name = info.spool_name;
            ovr.spoolman_id = info.spoolman_id;
            ovr.spoolman_vendor_id = info.spoolman_vendor_id;
            ovr.remaining_weight_g = info.remaining_weight_g;
            ovr.total_weight_g = info.total_weight_g;
            ovr.color_rgb = info.color_rgb;
            ovr.color_set = true; // a user-edit always records a color, even pure black (#000000)
            ovr.color_name = info.color_name;
            ovr.material = info.material;
            // Catalog product identity. Persisted so a reopen can restore the
            // EXACT product rather than the alphabetically-first variant of the
            // same vendor+material. Never auto-mirrored (firmware has no notion
            // of a catalog product), so no user-lock flag is needed: a non-empty
            // value can only have come from a user pick.
            ovr.catalog_id = info.catalog_id;
            ovr.product_name = info.product_name;
            // SlotInfo carries the user's edit OR the bound Spoolman spool's
            // filament profile; the material-DB fallback for fields left at 0
            // is applied at emit time inside resolved_temps(). Centralized in
            // the helper so the four AMS backends stay in sync.
            helix::ams::populate_temps_from_slot_info(ovr, info);
            // updated_at left default — save_async stamps a fresh value.
            overrides_[slot_index] = ovr;
        }
    }

    spdlog::info("[ACE] Updated slot {} info (persist={}): {} {}", slot_index, persist,
                 info.material, info.color_name);

    if (persist && override_store_) {
        // Re-read from overrides_ under the lock to pick up the staged copy.
        helix::ams::FilamentSlotOverride ovr_to_save;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = overrides_.find(slot_index);
            if (it != overrides_.end()) {
                ovr_to_save = it->second;
            }
        }
        // Capture by value — save_async's MR callback may fire long after
        // this returns (MR tracker ~60s timeout). Do NOT capture `this`:
        // the backend may outlive its store, but the store will outlive
        // the scheduled save by design.
        const std::string tag = backend_log_tag();
        override_store_->save_async(
            slot_index, ovr_to_save, [tag, slot_index](bool success, const std::string& err) {
                if (!success) {
                    spdlog::warn("{} Override persist failed for slot {}: {}", tag, slot_index,
                                 err);
                }
            });
    }

    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

AmsError AmsBackendAce::set_tool_mapping(int tool_number, int slot_index) {
    (void)tool_number;
    (void)slot_index;
    return AmsErrorHelper::not_supported("Tool mapping");
}

helix::printer::ToolMappingCapabilities AmsBackendAce::get_tool_mapping_capabilities() const {
    return {false, false, ""};
}

std::vector<int> AmsBackendAce::get_tool_mapping() const {
    return {};
}

// ============================================================================
// Bypass Mode (not supported)
// ============================================================================

AmsError AmsBackendAce::enable_bypass() {
    return AmsErrorHelper::not_supported("Bypass mode");
}

AmsError AmsBackendAce::disable_bypass() {
    return AmsErrorHelper::not_supported("Bypass mode");
}

bool AmsBackendAce::is_bypass_active() const {
    return false;
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
                        "Invalid temperature",
                        "Set temperature between " + std::to_string(static_cast<int>(min_temp)) +
                            "°C and " + std::to_string(static_cast<int>(max_temp)) + "°C");
    }

    if (duration_min <= 0 || duration_min > max_duration) {
        return AmsError(AmsResult::COMMAND_FAILED,
                        "Duration out of range: " + std::to_string(duration_min),
                        "Invalid duration",
                        "Set duration between 1 and " + std::to_string(max_duration) + " minutes");
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
        if (duration_min < 0)
            target_duration = dryer_info_.duration_min;
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

    // Parse status string -> AmsAction
    if (data.contains("status") && data["status"].is_string()) {
        std::string status_str = data["status"].get<std::string>();
        AmsAction action = AmsAction::IDLE;

        if (status_str == "loading") {
            action = AmsAction::LOADING;
        } else if (status_str == "unloading") {
            action = AmsAction::UNLOADING;
        } else if (status_str == "error") {
            action = AmsAction::ERROR;
        }
        // "ready", "drying", etc. -> IDLE

        system_info_.action = action;
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
                if (slot_json.contains("status") && slot_json["status"].is_string()) {
                    slot.status = slot_status_from_string(slot_json["status"].get<std::string>());
                }

                // Parse color: ValgACE returns [r, g, b] array
                if (slot_json.contains("color")) {
                    slot.color_rgb = parse_slot_color(slot_json["color"]);
                }

                // Parse material type (e.g., "PLA", "PETG")
                if (slot_json.contains("type") && slot_json["type"].is_string()) {
                    slot.material = slot_json["type"].get<std::string>();
                }

                // Parse SKU if present
                if (slot_json.contains("sku") && slot_json["sku"].is_string()) {
                    // SKU is available but not mapped to SlotInfo currently
                }

                // Hardware-event override clear. ACE has no RFID UID to track;
                // "user swapped the spool" is inferred from a status transition
                // EMPTY -> present. Must run BEFORE apply_overrides so the
                // clear sees firmware-truth (not the override-masked view) and
                // the reset of override-exclusive fields is visible in the
                // SlotInfo apply_overrides returns unchanged for cleared slots.
                //
                // First observation (no prev_slot_status_ entry) is a
                // BASELINE and must never fire a clear — matches IFS/Snapmaker
                // baseline semantics. Only call the helper when a prior status
                // was already recorded for this slot.
                int idx = static_cast<int>(i);
                auto prev_it = prev_slot_status_.find(idx);
                if (prev_it != prev_slot_status_.end()) {
                    check_hardware_event_clear(slot, idx, prev_it->second, slot.status);
                }
                prev_slot_status_[idx] = slot.status;

                // Layer user-configured overrides on top of firmware-reported
                // data. Override wins for any non-default field — for ACE
                // that includes color and material, since ACE hardware
                // doesn't carry brand/spool_name/weights at all and the user
                // edit is the authoritative source for color/material too.
                apply_overrides(slot, idx);
            }
        }
    }

    // Parse dryer state from combined object
    if (data.contains("dryer") && data["dryer"].is_object()) {
        const auto& dryer = data["dryer"];

        // ValgACE format: {status, target_temp, duration, remain_time}
        if (dryer.contains("status") && dryer["status"].is_string()) {
            std::string ds = dryer["status"].get<std::string>();
            dryer_info_.active = (ds != "stop" && ds != "idle" && !ds.empty());
        }
        if (dryer.contains("target_temp") && dryer["target_temp"].is_number()) {
            dryer_info_.target_temp_c = dryer["target_temp"].get<float>();
        }
        if (dryer.contains("duration") && dryer["duration"].is_number()) {
            dryer_info_.duration_min = dryer["duration"].get<int>();
        }
        if (dryer.contains("remain_time") && dryer["remain_time"].is_number()) {
            dryer_info_.remaining_min = dryer["remain_time"].get<int>();
        }

        // Also accept REST-bridge format keys for compatibility
        if (dryer.contains("active") && dryer["active"].is_boolean()) {
            dryer_info_.active = dryer["active"].get<bool>();
        }
        if (dryer.contains("current_temp") && dryer["current_temp"].is_number()) {
            dryer_info_.current_temp_c = dryer["current_temp"].get<float>();
        }
        if (dryer.contains("remaining_minutes") && dryer["remaining_minutes"].is_number_integer()) {
            dryer_info_.remaining_min = dryer["remaining_minutes"].get<int>();
        }
        if (dryer.contains("duration_minutes") && dryer["duration_minutes"].is_number_integer()) {
            dryer_info_.duration_min = dryer["duration_minutes"].get<int>();
        }
    }

    // Parse temperature from top-level (ACE ambient temp near dryer)
    if (data.contains("temp") && data["temp"].is_number()) {
        dryer_info_.current_temp_c = data["temp"].get<float>();
    }

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
    bool found_loaded = false;
    if (!system_info_.units.empty()) {
        for (int i = 0; i < static_cast<int>(system_info_.units[0].slots.size()); ++i) {
            // Check the raw JSON for "loaded" status specifically
            if (data.contains("slots") && data["slots"].is_array() &&
                i < static_cast<int>(data["slots"].size())) {
                const auto& sj = data["slots"][static_cast<size_t>(i)];
                if (sj.contains("status") && sj["status"].is_string() &&
                    sj["status"].get<std::string>() == "loaded") {
                    system_info_.current_slot = i;
                    system_info_.current_tool = i;
                    system_info_.filament_loaded = true;
                    found_loaded = true;
                    break;
                }
            }
        }
    }

    // Also handle explicit loaded_slot if present (future compatibility)
    if (data.contains("loaded_slot") && data["loaded_slot"].is_number_integer()) {
        int slot = data["loaded_slot"].get<int>();
        system_info_.current_slot = slot;
        system_info_.current_tool = slot;
        system_info_.filament_loaded = (slot >= 0);
        found_loaded = (slot >= 0);
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
                    system_info_.current_slot = local_index;
                    system_info_.current_tool = local_index;
                    system_info_.filament_loaded = true;
                    found_loaded = true;
                }
            } catch (const std::exception& e) {
                spdlog::debug("[ACE] Failed to parse current_filament '{}': {}", cf, e.what());
            }
        }
    }

    if (!found_loaded && !data.contains("loaded_slot")) {
        // No slot is in "loaded" state and no explicit loaded_slot field
        // Keep existing loaded state unless status indicates otherwise
        if (data.contains("status") && data["status"].is_string()) {
            std::string s = data["status"].get<std::string>();
            if (s == "ready" && system_info_.action == AmsAction::IDLE) {
                // "ready" with no loaded slot means nothing loaded
                // But only reset if we haven't seen loaded state from other sources
            }
        }
    }

    // All three seated signals (the ValgACE "loaded" scan, loaded_slot, and
    // native current_filament) have now had their say and arbitrated to one
    // slot; publish that as the slot's own status.
    apply_seated_slot_stamp_locked();
}

const json* AmsBackendAce::select_slot_bearing_object(const json& status,
                                                      std::string* matched_key) {
    // Commit to the subscription path ONLY when the object actually carries
    // slot data (a non-empty "slots" array — the exact key parse_ace_object
    // reads). A manager-only object (Kobra S1 fork's `ace`: ace_instances /
    // current_index, no slots) has no slots array and must fall through to the
    // REST bridge so the whole /server/ace/* surface gets queried (#1069).
    auto has_slot_data = [](const json& obj) {
        return obj.is_object() && obj.contains("slots") && obj["slots"].is_array() &&
               !obj["slots"].empty();
    };

    // Preference order: filament_hub (native GoKlipper), then ace (community
    // ValgACE/BunnyACE), then ace_instance_N (Kobra S1 mainline-Python fork —
    // #1107) in ascending name order. The object path is used only if the
    // matched object carries a slots array; otherwise the caller falls through
    // to the REST bridge.
    if (status.contains("filament_hub") && has_slot_data(status["filament_hub"])) {
        if (matched_key)
            *matched_key = "filament_hub";
        return &status["filament_hub"];
    }
    if (status.contains("ace") && has_slot_data(status["ace"])) {
        if (matched_key)
            *matched_key = "ace";
        return &status["ace"];
    }
    // Kobra S1 fork registers each unit as `ace_instance_N`. Pick the
    // lowest-numbered slot-bearing instance so the choice is deterministic.
    if (status.is_object()) {
        const std::string* best_key = nullptr;
        for (auto it = status.begin(); it != status.end(); ++it) {
            if (it.key().rfind("ace_instance", 0) == 0 && has_slot_data(it.value())) {
                if (best_key == nullptr || it.key() < *best_key) {
                    best_key = &it.key();
                }
            }
        }
        if (best_key) {
            if (matched_key)
                *matched_key = *best_key;
            return &status[*best_key];
        }
    }
    return nullptr;
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

uint32_t AmsBackendAce::parse_slot_color(const json& color_val) {
    // ValgACE format: [r, g, b] array
    if (color_val.is_array() && color_val.size() >= 3) {
        try {
            uint8_t r = static_cast<uint8_t>(color_val[0].get<int>());
            uint8_t g = static_cast<uint8_t>(color_val[1].get<int>());
            uint8_t b = static_cast<uint8_t>(color_val[2].get<int>());
            return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
                   static_cast<uint32_t>(b);
        } catch (const std::exception& e) {
            spdlog::debug("[ACE] Failed to parse color array: {}", e.what());
            return 0;
        }
    }

    // REST bridge format: hex string "#RRGGBB" or "0xRRGGBB"
    if (color_val.is_string()) {
        std::string color_str = color_val.get<std::string>();
        if (!color_str.empty()) {
            try {
                if (color_str[0] == '#') {
                    color_str = color_str.substr(1);
                } else if (color_str.size() > 2 && color_str[0] == '0' &&
                           (color_str[1] == 'x' || color_str[1] == 'X')) {
                    color_str = color_str.substr(2);
                }
                return static_cast<uint32_t>(std::stoul(color_str, nullptr, 16));
            } catch (const std::exception& e) {
                spdlog::debug("[ACE] Failed to parse color string '{}': {}", color_str, e.what());
            }
        }
    }

    return 0;
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
        int slot = data["loaded_slot"].get<int>();
        if (slot != system_info_.current_slot) {
            system_info_.current_slot = slot;
            system_info_.current_tool = slot;
            changed = true;
        }

        bool loaded = (slot >= 0);
        if (loaded != system_info_.filament_loaded) {
            system_info_.filament_loaded = loaded;
            changed = true;
        }
    }

    if (data.contains("action") && data["action"].is_string()) {
        std::string action_str = data["action"].get<std::string>();
        AmsAction action = AmsAction::IDLE;

        if (action_str == "loading") {
            action = AmsAction::LOADING;
        } else if (action_str == "unloading") {
            action = AmsAction::UNLOADING;
        } else if (action_str == "error") {
            action = AmsAction::ERROR;
        } else if (action_str == "drying") {
            action = AmsAction::IDLE;
        }

        if (action != system_info_.action) {
            system_info_.action = action;
            changed = true;
        }
    }

    if (data.contains("dryer") && data["dryer"].is_object()) {
        const auto& dryer = data["dryer"];

        if (dryer.contains("active") && dryer["active"].is_boolean()) {
            dryer_info_.active = dryer["active"].get<bool>();
        }
        if (dryer.contains("current_temp") && dryer["current_temp"].is_number()) {
            dryer_info_.current_temp_c = dryer["current_temp"].get<float>();
        }
        if (dryer.contains("target_temp") && dryer["target_temp"].is_number()) {
            dryer_info_.target_temp_c = dryer["target_temp"].get<float>();
        }
        if (dryer.contains("remaining_minutes") && dryer["remaining_minutes"].is_number_integer()) {
            dryer_info_.remaining_min = dryer["remaining_minutes"].get<int>();
        }
        if (dryer.contains("duration_minutes") && dryer["duration_minutes"].is_number_integer()) {
            dryer_info_.duration_min = dryer["duration_minutes"].get<int>();
        }
    }

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

        if (slot_json.contains("status") && slot_json["status"].is_string()) {
            // Mirror the object path exactly via the shared vocabulary map —
            // this fork emits `ready` (and can emit `unknown`), which the old
            // empty/available/loaded-only mapping misclassified (#1069).
            SlotStatus status = slot_status_from_string(slot_json["status"].get<std::string>());

            if (status != slot.status) {
                slot.status = status;
                changed = true;
            }
        }

        // Parse color: handle both hex string and RGB array formats
        if (slot_json.contains("color")) {
            uint32_t color = parse_slot_color(slot_json["color"]);
            if (color != slot.color_rgb) {
                slot.color_rgb = color;
                changed = true;
            }
        }

        // Material: prefer `material`, fall back to `type` (this fork's /slots
        // returns `type`, like the object path). Mirror parse_ace_object.
        std::string material;
        if (slot_json.contains("material") && slot_json["material"].is_string()) {
            material = slot_json["material"].get<std::string>();
        }
        if (material.empty() && slot_json.contains("type") && slot_json["type"].is_string()) {
            material = slot_json["type"].get<std::string>();
        }
        if (!material.empty() && material != slot.material) {
            slot.material = material;
            changed = true;
        }

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
// Helpers
// ============================================================================

AmsError AmsBackendAce::validate_slot_index(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (slot_index < 0 || slot_index >= system_info_.total_slots) {
        return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
    }

    return AmsErrorHelper::success();
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

void AmsBackendAce::apply_overrides(SlotInfo& slot, int slot_index) {
    // overrides_ writers (on_started initial load, set_slot_info persist path)
    // hold mutex_, and every caller of apply_overrides runs under mutex_ via
    // parse_ace_object — so the map read here is implicitly lock-protected.
    // The whole spec §5 policy + the re-bind/eject rules live in
    // helix::ams::merge_override — the single implementation every backend
    // shares. Rule 1 (re-bind) is NOT gated by the capability: it can fire
    // on any backend whose firmware reports a positive spool id disagreeing
    // with the override (AFC, Happy Hare, flat-schema CFS). ACE's firmware
    // never reports one, so Rule 1 cannot fire here today — but that is a
    // fact about this firmware, not what the capability gates. Rule 2
    // (eject) IS what printer_reports_spool_ids() gates (base false here:
    // 0 is ACE's everyday reading, never an eject), and the erase branch is
    // correct tomorrow if a firmware ever starts reporting ids.
    auto it = overrides_.find(slot_index);
    if (it == overrides_.end())
        return;
    helix::ams::MergeOptions opts;
    opts.printer_reports_spool_ids = printer_reports_spool_ids();
    opts.keep_spool_info_on_eject = SettingsManager::instance().get_ams_keep_spool_info_on_eject();
    // Own-write echo suppression (SlotFingerprintTracker::expect()
    // semantics): Rule 1 must not read an in-flight stale firmware id as an
    // external re-bind. ACE never writes firmware ids, so this is always
    // {0, 0} today — the call keeps one shape across backends.
    const auto [own_old_id, own_new_id] = own_write_expectation(slot_index, slot.spoolman_id);
    opts.suppress_rebind_firmware_old_id = own_old_id;
    opts.suppress_rebind_firmware_new_id = own_new_id;
    const auto result = helix::ams::merge_override(slot, it->second, opts);
    if (result.cleared_rebind || result.cleared_eject) {
        overrides_.erase(it);
        if (override_store_) {
            const std::string tag = backend_log_tag();
            override_store_->clear_async(slot_index, [tag, slot_index](bool ok, std::string err) {
                if (!ok) {
                    spdlog::warn("{} clear_async failed for slot {}: {}", tag, slot_index, err);
                }
            });
        }
    }
}

void AmsBackendAce::check_hardware_event_clear(SlotInfo& slot, int slot_index, SlotStatus prev,
                                               SlotStatus curr) {
    // ACE has no RFID UID to track. Detect "new spool inserted" as a status
    // transition from EMPTY -> present (AVAILABLE / LOADED). A LOADED ->
    // EMPTY transition is NOT a swap — the user may reinsert the same spool.
    // UNKNOWN is treated as "no signal" on either side and never fires the
    // check; callers must only invoke this helper AFTER a valid prior
    // observation has been recorded (caller handles the baseline skip).
    const bool was_empty = (prev == SlotStatus::EMPTY);
    const bool is_present = (curr == SlotStatus::AVAILABLE || curr == SlotStatus::LOADED);
    if (!was_empty || !is_present)
        return;

    auto ovr_it = overrides_.find(slot_index);
    if (ovr_it == overrides_.end()) {
        spdlog::debug("[ACE] Slot {} insertion detected (prev={}, curr={}); "
                      "no override to clear",
                      slot_index, slot_status_to_string(prev), slot_status_to_string(curr));
        return;
    }

    spdlog::info("[ACE] Slot {} insertion detected (prev={}, curr={}); clearing override",
                 slot_index, slot_status_to_string(prev), slot_status_to_string(curr));

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

    slot.brand.clear();
    slot.spool_name.clear();
    slot.spoolman_id = 0;
    slot.spoolman_vendor_id = 0;
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
