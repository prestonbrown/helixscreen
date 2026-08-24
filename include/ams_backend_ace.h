// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"
#include "async_lifetime_guard.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "moonraker_types.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

/**
 * @file ams_backend_ace.h
 * @brief ACE (Anycubic ACE Pro) backend implementation
 *
 * Implements the AmsBackend interface for AnyCubic ACE Pro systems
 * using the ValgACE/BunnyACE/DuckACE Klipper drivers.
 *
 * Primary path (ValgACE): Subscribes to the `ace` Klipper object via
 * standard Moonraker WebSocket. ValgACE implements get_status() which
 * returns combined state in a single object.
 *
 * Fallback path (BunnyACE/DuckACE): If the initial query returns empty
 * data (driver lacks get_status()), falls back to REST polling via the
 * ace_status.py Moonraker bridge at /server/ace/ endpoints.
 *
 * G-code Commands:
 * - ACE_CHANGE_TOOL TOOL={n}  - Load filament from slot n (-1 to unload)
 * - ACE_START_DRYING TEMP={t} DURATION={m}  - Start drying
 * - ACE_STOP_DRYING           - Stop drying
 *
 * Thread Model:
 * - Primary: WebSocket subscription callbacks on background thread
 * - Fallback: REST polling thread at ~500ms interval
 * - State is cached under mutex protection (inherited from AmsSubscriptionBackend)
 */
class AceTestAccess;

class AmsBackendAce : public AmsSubscriptionBackend {
  public:
    AmsBackendAce(IMoonrakerAPI* api, helix::IMoonrakerClient* client);

    ~AmsBackendAce() override;

    // ========================================================================
    // Type
    // ========================================================================

    [[nodiscard]] AmsType get_type() const override {
        return AmsType::ACE;
    }

    // ACE marker for expected-hardware recording during wizard setup. ACE is
    // REST-based, not a real Klipper object, but the validator keys on this name.
    [[nodiscard]] const char* get_klipper_object_name() const override {
        return "ace";
    }

    // ACE uses ACE_CHANGE_TOOL TOOL=n, not the U1 Tn/SM_PRINT_* families
    // GcodeToolRemapper handles. Remap disabled until that command family is
    // implemented + validated on a real ACE file.
    [[nodiscard]] RemapStrategy get_remap_strategy() const override {
        return RemapStrategy::None;
    }

    // ========================================================================
    // State Queries
    // ========================================================================

    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;

    // ========================================================================
    // Path Visualization
    // ========================================================================

    [[nodiscard]] PathTopology get_topology() const override;
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    /// Every parse path that resolves the seated slot now stamps
    /// SlotStatus::LOADED on it (see apply_seated_slot_stamp_locked), so the
    /// per-slot status answers the per-slot question. Before that the only
    /// LOADED write lived in load_filament()'s gcode-success callback and the
    /// next status frame erased it, leaving the inherited
    /// can_unload_from_toolhead() false on every ACE slot (#1199).
    [[nodiscard]] bool has_per_slot_loaded_authority() const override {
        return true;
    }

    // ========================================================================
    // Filament Operations
    // ========================================================================

  protected:
    // Gated by AmsSubscriptionBackend's NVI wrapper — these run only once the
    // print-active check has passed.
    AmsError do_load_filament(int slot_index) override;
    AmsError do_unload_filament(int slot_index) override;
    AmsError do_select_slot(int slot_index) override;
    AmsError do_change_tool(int tool_number) override;

    /// ACE has no "select without loading": do_select_slot() forwards to
    /// do_load_filament(), so a select pushes filament through the hotend.
    [[nodiscard]] bool select_slot_moves_toolhead() const override {
        return true;
    }

  public:
    // ========================================================================
    // Recovery Operations
    // ========================================================================

    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;

    // ========================================================================
    // Configuration
    // ========================================================================

    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;

    // Explicit user-initiated override clear (e.g. "Clear slot metadata" button
    // in the AMS edit modal). Erases overrides_[slot_index], resets the
    // override-exclusive fields on the live SlotInfo, and fires
    // override_store_->clear_async. ACE firmware doesn't populate brand /
    // spool_name / spoolman_* / weights / color_name — those are override-only,
    // so they're all zeroed. Color/material come from firmware so they stay.
    // The hardware-event detector calls this internally once an EMPTY -> present
    // transition confirms a physical swap.
    void clear_slot_override(int slot_index) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    // ACE has fixed 1:1 mapping (tools ARE slots), not configurable
    [[nodiscard]] helix::printer::ToolMappingCapabilities
    get_tool_mapping_capabilities() const override;
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    // ========================================================================
    // Bypass Mode (not supported on ACE Pro)
    // ========================================================================

    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override;

    // ========================================================================
    // Environment Sensors & Dryer Control (ACE Pro has built-in dryer + temp)
    // ========================================================================

    [[nodiscard]] bool has_environment_sensors() const override {
        return true;
    }
    [[nodiscard]] DryerInfo get_dryer_info(int unit = 0) const override;
    AmsError start_drying(float temp_c, int duration_min, int fan_pct = -1, int unit = 0) override;
    AmsError stop_drying(int unit = 0) override;
    AmsError update_drying(float temp_c = -1, int duration_min = -1, int fan_pct = -1,
                           int unit = 0) override;
    [[nodiscard]] std::vector<DryingPreset> get_drying_presets() const override;

    // ========================================================================
    // Device Actions
    // ========================================================================

    [[nodiscard]] std::vector<helix::printer::DeviceSection> get_device_sections() const override;
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;
    AmsError execute_device_action(const std::string& action_id,
                                   const std::any& value = {}) override;

  protected:
    // ========================================================================
    // AmsSubscriptionBackend hooks
    // ========================================================================

    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override {
        return "[ACE]";
    }
    void on_started() override;
    void on_stopping() override;

    // ========================================================================
    // Response Parsing (protected for unit testing)
    // ========================================================================

    /**
     * @brief Parse /server/ace/info response (REST fallback path)
     * @param data JSON response data
     */
    void parse_info_response(const nlohmann::json& data);

    /**
     * @brief Parse /server/ace/status response (REST fallback path)
     * @param data JSON response data
     * @return true if state changed (emit event)
     */
    bool parse_status_response(const nlohmann::json& data);

    /**
     * @brief Parse /server/ace/slots response (REST fallback path)
     * @param data JSON response data
     * @return true if state changed (emit event)
     */
    bool parse_slots_response(const nlohmann::json& data);

    /**
     * @brief Parse combined ace Klipper object data (WebSocket subscription path)
     *
     * ValgACE's get_status() returns all state in one object: model, firmware,
     * status, slots, dryer, etc. This handles that combined format.
     *
     * @param data JSON data from the ace Klipper object
     */
    void parse_ace_object(const nlohmann::json& data);

  private:
    friend class ::AceTestAccess;

    // ========================================================================
    // REST Fallback (for BunnyACE/DuckACE without get_status())
    // ========================================================================

    void start_rest_fallback();
    void stop_rest_fallback();
    void rest_polling_loop();

    void poll_info();
    void poll_status();
    void poll_slots();

    // ========================================================================
    // Helpers
    // ========================================================================

    AmsError validate_slot_index(int slot_index) const;

    /**
     * @brief Parse slot color from either RGB array [r,g,b] or hex string "#RRGGBB"
     * @param color_val JSON value (array or string)
     * @return Parsed RGB color value
     */
    static uint32_t parse_slot_color(const nlohmann::json& color_val);

    /**
     * @brief Pick the ace/filament_hub status object that actually carries slot
     *        data (a non-empty "slots" array).
     *
     * The on_started() query commits to the WebSocket subscription path only
     * when the matched object carries slots. A manager-only object — e.g. the
     * Kobra S1 fork's `ace` object exposes `ace_instances`/`current_index` but
     * NO `slots` (the per-unit slot data lives in separate `ace_instance_N`
     * objects) — must fall through to the REST bridge at /server/ace/ instead
     * of parsing zero slots off the manager (#1069).
     *
     * @param status The `result.status` object from printer.objects.query
     * @param matched_key Out: set to the picked key ("filament_hub"/"ace"/
     *        "ace_instance_N") when a slot-bearing object is found; left
     *        unchanged otherwise. May be null. Owned std::string so the key
     *        stays valid regardless of the source json's lifetime (the
     *        `ace_instance_N` keys are dynamic, not string literals).
     * @return Pointer to the slot-bearing object (borrowed from @p status), or
     *         nullptr if no filament_hub/ace/ace_instance_N carries a slots
     *         array.
     */
    static const nlohmann::json* select_slot_bearing_object(const nlohmann::json& status,
                                                            std::string* matched_key);

    /**
     * @brief Map an ACE slot status string to a SlotStatus.
     *
     * Single source of truth shared by the WebSocket object path
     * (parse_ace_object) and the REST fallback path (parse_slots_response) so
     * the two vocabularies can't drift. empty/runout -> EMPTY;
     * available/loaded/ready/preload/running -> AVAILABLE; anything else
     * (including "unknown") -> UNKNOWN.
     */
    static SlotStatus slot_status_from_string(const std::string& status_str);

    // ========================================================================
    // Members
    // ========================================================================

    // Dryer state (ACE-specific, not in base class)
    DryerInfo dryer_info_;

    // Info tracking
    std::atomic<bool> info_fetched_{false};
    std::atomic<int> info_fetch_failures_{0};

    // Data-endpoint (/status + /slots) tracking. /server/ace/info is optional —
    // model/slots come from /status + /slots — so the "bridge not found" error
    // is gated on the DATA endpoints failing, not on /info (#1069). rest_data_ok_
    // latches true once /status or /slots ever succeeds; data_fetch_failures_
    // counts consecutive /status failures (reset by any /status OR /slots
    // success) and drives the one-shot error toast only while rest_data_ok_ is
    // still false.
    std::atomic<bool> rest_data_ok_{false};
    std::atomic<int> data_fetch_failures_{0};

    // Callback lifetime management
    helix::AsyncLifetimeGuard lifetime_;

    // REST fallback state
    bool use_rest_fallback_{false};
    std::thread rest_polling_thread_;
    std::atomic<bool> rest_stop_requested_{false};
    std::condition_variable rest_stop_cv_;
    std::mutex rest_stop_mutex_;

    // Configuration
    static constexpr int POLL_INTERVAL_MS = 500;
    /// Consecutive data-endpoint (/status) failures before surfacing the
    /// "Moonraker bridge not found" error (genuinely-missing-bridge case).
    static constexpr int MAX_DATA_FETCH_FAILURES = 3;

    // Layer any configured FilamentSlotOverride for `slot_index` over `slot`,
    // mutating `slot` in place. Override wins for every non-default field;
    // default values (empty string, 0, -1.0 weights) fall through to the
    // firmware-reported data untouched. Called from parse_ace_object so every
    // parse path picks up the override before the SlotInfo is exposed via
    // events. ACE hardware doesn't carry brand/spool/weights, so the override
    // is the only source for those fields; color/material come from both the
    // firmware and user edits and the override wins per the merge policy.
    void apply_overrides(SlotInfo& slot, int slot_index);

    // Hardware-event detection: ACE has no RFID UID, so "user physically
    // swapped the spool" is inferred from a status transition EMPTY -> present
    // (AVAILABLE / LOADED). When detected, the stored override for the slot
    // is cleared so stale brand/spool_name/spoolman_id from the previous
    // spool don't bleed onto the new one. Override-exclusive fields on `slot`
    // are zeroed in place so the cleared state is visible in the very next
    // get_slot_info() read (apply_overrides then no-ops for that slot).
    //
    // Called from parse_ace_object BEFORE apply_overrides, so the check
    // decides based on parsed firmware status (not override-masked data). The
    // caller is responsible for skipping the very first observation (no prior
    // prev_slot_status_ entry) — first-observation is a baseline and never
    // fires. Limitation: a LOADED -> EMPTY -> LOADED sequence (user unloaded
    // and reinserted the same spool) looks identical to a swap under this
    // status-based heuristic and clears the override. Documented tradeoff —
    // ACE's single signal is too coarse to distinguish the two cases.
    void check_hardware_event_clear(SlotInfo& slot, int slot_index, SlotStatus previous_status,
                                    SlotStatus current_status);

    /// Mutable slot lookup. ACE is always single-unit, and the two REST
    /// parsers size units[0].slots independently, so index directly rather
    /// than through AmsSystemInfo::get_slot_global() — that walks
    /// first_slot_global_index/slot_count, which parse_slots_response only
    /// refreshes when the slot count actually changes. Caller holds mutex_.
    SlotInfo* mutable_slot_locked(int slot_index);

    /// Undo the derived LOADED stamp, restoring the status the last parse
    /// wrote. Caller holds mutex_. Runs at the TOP of every parse so
    /// check_hardware_event_clear, prev_slot_status_ and
    /// parse_slots_response's `status != slot.status` change detection all see
    /// firmware truth — without it, /slots would report a change on every
    /// 500 ms poll forever.
    void clear_seated_slot_stamp_locked();

    /// Re-derive the LOADED stamp from the arbitrated aggregate and apply it.
    /// Caller holds mutex_. Runs at the BOTTOM of every parse, and after the
    /// optimistic aggregate writes in load_filament()/unload_filament().
    ///
    /// Deliberately keyed on the aggregate rather than on the per-slot status
    /// string. Native GoKlipper's per-slot vocabulary
    /// (empty/ready/preload/running/runout) has no seated state at all — it
    /// answers that with the separate top-level `current_filament` — and
    /// community ValgACE's "loaded" sits in the same enumeration as
    /// "available"/"ready", the same slot-local trap as AFC's lane status
    /// "Loaded" meaning loaded-to-hub. slot_status_from_string() therefore
    /// stays as it is; the seated slot is whichever one parse_ace_object /
    /// parse_status_response arbitrated to, and a HUB backend has exactly one.
    void apply_seated_slot_stamp_locked();

    /// Slot index the LOADED stamp currently sits on, and the status the parse
    /// had written there before it was overwritten. -1 / UNKNOWN when no stamp
    /// is outstanding.
    int seated_stamp_slot_ = -1;
    SlotStatus seated_stamp_prev_ = SlotStatus::UNKNOWN;

    // Shared helper used by every override-clear path (hardware event and
    // explicit user request). Caller must hold mutex_. Erases
    // overrides_[slot_index], resets override-exclusive fields on the
    // provided SlotInfo (brand, spool_name, spoolman_*, weights, color_name),
    // and fires clear_async. Color/material stay untouched — firmware owns
    // them for ACE and the parse has just refreshed them.
    void clear_override_locked(int slot_index, SlotInfo& slot);

    // User-provided per-slot metadata (brand, spool name, spoolman IDs,
    // remaining weight, etc.) layered over firmware-reported state.
    // Both writers (on_started initial load, set_slot_info persist path) hold
    // mutex_; apply_overrides reads inside the parse path under mutex_.
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> override_store_;
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides_;

    // Previous slot status per slot index. Used as the swap-detection signal:
    // an EMPTY -> present transition fires the clear-override path. Map
    // presence also acts as the baseline guard: absent entry means "no prior
    // observation" and the check is skipped (first observation never clears).
    // Access is always under mutex_ (parse_ace_object is the only
    // writer/reader).
    std::unordered_map<int, SlotStatus> prev_slot_status_;
};
