// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#if HELIX_HAS_ACE

#include "ams_bypass_policy.h"
#include "ams_subscription_backend.h"
#include "async_lifetime_guard.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_binding.h"
#include "moonraker_types.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace helix {

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

    AmsError apply_user_edit(int slot_index, const SlotInfo& info,
                             const helix::ams::Observation& declared) override;
    AmsError sync_external_identity(int slot_index, const SlotInfo& info) override;
    void persist_slot_weight(int slot_index, float remaining_weight_g,
                             float total_weight_g) override;
    void persist_external_identity_impl(int slot_index,
                                        const helix::ams::Observation& spoolman) override;

    // Explicit user-initiated override clear (e.g. "Clear slot metadata" button
    // in the AMS edit modal). Erases overrides_[slot_index], resets the
    // override-exclusive fields on the live SlotInfo, and fires
    // override_store_->clear_async. ACE firmware doesn't populate brand /
    // spool_name / spoolman_* / weights / color_name — those are override-only,
    // so they're all zeroed. Color/material come from firmware so they stay.
    // The hardware-event detector calls this internally once an EMPTY -> present
    // transition confirms a physical swap.
    void clear_slot_override(int slot_index) override;
    AmsError set_tool_mapping_impl(int tool_number, int slot_index) override;

    // ACE has fixed 1:1 mapping (tools ARE slots), not configurable — it
    // declares RemapStrategy::None and owns no tool->slot table.
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    // ========================================================================
    // Bypass Mode
    //
    // The ACE Pro has no bypass selector of its own. What some rigs have is a
    // master switch that disables the whole ACE path so a fifth spool can be
    // fed to the toolhead by hand, published as `ace_pro_enabled`. Its
    // PRESENCE is the capability; its VALUE is the state, inverted — the ACE
    // path being off is what bypass means here
    // (prestonbrown/helixscreen#1677).
    //
    // There is no native command for it, so the switch is thrown by macros.
    // Driving the underlying pin directly would skip the unload-first and
    // refuse-during-print guards those macros exist to enforce, so a rig that
    // names no macros reports no bypass rather than offering a control with
    // nothing safe behind it.
    // ========================================================================

    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override;

    /// The macros that throw the ACE master switch, resolved from discovery
    /// with a user override. Empty names mean this rig cannot bypass.
    void set_bypass_macros(helix::BypassMacros macros) override;

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
    friend class AceTestAccess;

    // ========================================================================
    // REST Fallback (for BunnyACE/DuckACE without get_status())
    // ========================================================================

    void start_rest_fallback();
    void stop_rest_fallback();
    void rest_polling_loop();

    void poll_info();
    void poll_status();
    void poll_slots();

    /**
     * @brief Parse slot color from either RGB array [r,g,b] or hex string "#RRGGBB"
     *
     * Empty when the value carries no colour: a short or non-numeric array, a
     * string the shared lane colour grammar refuses, any other JSON type. That
     * is a different answer from pure black, which ACE reports for an empty bay
     * and which is a real reading, so the two must not share a return value.
     *
     * @param color_val JSON value (array or string)
     * @return Parsed RGB color value, or nullopt when the value states none
     */
    static std::optional<uint32_t> parse_slot_color(const nlohmann::json& color_val);

    /**
     * @brief Read one slot's material, for both slot-bearing producers (the
     *        WebSocket object path and the REST /slots poll read the same hub).
     *
     * `material` is the Kobra S1 fork/native live spelling, `type` the
     * ValgACE spelling; material wins when both are stated. An empty or
     * absent value states no reading.
     *
     * @return The material, or nullopt when the slot states none
     */
    static std::optional<std::string> read_slot_material(const nlohmann::json& slot_json);

    /**
     * @brief Pick the ace/filament_hub/ace_instance_N object to parse from a
     *        status frame, by one preference order.
     *
     * Both selection passes share this so the key list cannot drift between
     * them. The predicate varies by caller: with @p require_slots the object
     * must carry a non-empty "slots" array — the on_started() query commits to
     * the WebSocket subscription path only on real slot data, so a manager-only
     * object (the Kobra S1 fork's `ace` exposes `ace_instances`/`current_index`
     * but NO `slots`; the per-unit data lives in `ace_instance_N`) falls
     * through to the REST bridge at /server/ace/ instead of parsing zero slots
     * off the manager (#1069). Without it, any non-empty object qualifies —
     * notify frames carry only CHANGED fields, so a legitimate delta (e.g. a
     * native `filament_hub` restating `current_filament`) has no slots array;
     * @p skip excludes an object the caller parses separately (the manager).
     *
     * @param status The `result.status` object from printer.objects.query, or
     *        one notify_status_update frame's status object
     * @param matched_key Out: set to the picked key ("filament_hub"/"ace"/
     *        "ace_instance_N") when an object matches; left unchanged
     *        otherwise. May be null. Owned std::string so the key stays valid
     *        regardless of the source json's lifetime (the `ace_instance_N`
     *        keys are dynamic, not string literals).
     * @param require_slots Require a non-empty "slots" array on the pick
     * @param skip Never pick this object (pointer into @p status; may be null)
     * @return Pointer to the picked object (borrowed from @p status), or
     *         nullptr when nothing matches
     */
    static const nlohmann::json* select_ace_object(const nlohmann::json& status,
                                                   std::string* matched_key,
                                                   bool require_slots = true,
                                                   const nlohmann::json* skip = nullptr);

    /**
     * @brief The lowest-numbered `ace_instance_N` key whose value satisfies
     *        @p predicate, so every walk over the fork's per-unit objects
     *        picks the same one.
     *
     * The display is anchored to the lowest instance — on_started's
     * slot-bearing pick populates it — so notify deltas must resolve to it
     * too, not to whichever instance happened to carry the slots array whole
     * in a given frame (#1107).
     *
     * @param status A query result.status object or one notify frame's status
     * @param predicate Object filter; defaults to "non-empty object", the
     *        notify-delta shape. Borrowed by select_ace_object with its own
     *        mode-specific filter.
     * @return Pointer to the key (borrowed from @p status), or null
     */
    static const std::string* lowest_ace_instance_key(
        const nlohmann::json& status,
        const std::function<bool(const nlohmann::json&)>& predicate = [](const nlohmann::json& o) {
            return o.is_object() && !o.empty();
        });

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

    /**
     * @brief The manager-shaped `ace` object in a status frame, when one is
     *        present.
     *
     * The Kobra S1 fork splits its surface: each unit's slots live on
     * `ace_instance_N`, while the top-level `ace` is a manager whose only
     * seat signal is `current_index` (#1069). A slot-bearing `ace` (ValgACE)
     * is NOT a manager — returning null for it keeps the caller from parsing
     * the same object twice.
     *
     * @param status A printer.objects.query result.status object, or one
     *        notify_status_update frame's status object
     * @return Pointer to the manager object (borrowed from @p status), or
     *         null when no manager-shaped `ace` is present
     */
    static const nlohmann::json* manager_ace_object(const nlohmann::json& status);

    /// Fold one status-shaped object's dryer state into dryer_info_ under
    /// either spelling: `dryer` (ValgACE/native) or `dryer_status` (Kobra S1
    /// fork), each with either nested key set ({status, target_temp,
    /// duration, remain_time} or {active, current_temp, remaining_minutes,
    /// duration_minutes}). Also reads the top-level ambient `temp`, which a
    /// stated dryer current temp overrides. Caller holds mutex_.
    void apply_dryer_state_locked(const nlohmann::json& data);

    /// Fold the manager's path sensors (`rdm_sensor` at the hub,
    /// `toolhead_sensor` at the extruder) into the cached readings and onto the
    /// unit. A frame stating neither leaves the last reading standing: notify
    /// frames carry only changed fields, so silence is not a cleared sensor.
    /// Caller holds mutex_.
    void apply_path_sensors_locked(const nlohmann::json& data);

    /// Seat the loaded tool from the fork manager's `current_index` — the
    /// global tool index across every unit, -1 = nothing loaded. This backend
    /// displays one unit: below that unit's slot count the global index IS
    /// the local slot (delegates to seat_from_local_index_locked); at or
    /// beyond it the seat lives in a unit that is not displayed, so the tool
    /// is stated and no slot is marked (the stamp helper guards
    /// current_slot < 0). Caller holds mutex_.
    /// @return true when current_slot/current_tool/filament_loaded changed
    bool seat_from_global_index_locked(int current_index);

    /// Seat the loaded tool from a LOCAL slot index (loaded_slot, the
    /// ValgACE "loaded" scan, current_filament's parsed local index): -1
    /// clears, otherwise the index is the slot and the tool. Caller holds
    /// mutex_.
    /// @return true when current_slot/current_tool/filament_loaded changed
    bool seat_from_local_index_locked(int slot_index);

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

    // Insert-edge verdict (prestonbrown/helixscreen#1710). The status
    // transition EMPTY -> present (AVAILABLE / LOADED) is "a spool was put
    // in"; what that insert does to the stored override is the insert rule's
    // call on the two tag readings: DifferentSpool clears the override (via
    // clear_override_locked, so stale brand/spool_name/spoolman_id from the
    // previous spool don't bleed onto the new one), SameSpool keeps it, and
    // NoEvidence keeps it and offers the same-spool notice on the UI thread.
    //
    // The frame that reports the spool can precede the one carrying its tag
    // read, so an insert edge whose frame carries no read does not judge: it
    // arms pending_insert_reads_, the first rfid-bearing frame for the bay
    // judges it, and a read that never lands expires to the no-evidence
    // notice after kAcePendingReadParsePasses parse passes. A bay that
    // empties with an insert still pending drops it.
    //
    // Called from parse_ace_object BEFORE apply_resolved_lane, so the check
    // decides based on parsed firmware status (not the resolved view). The
    // caller is responsible for skipping the very first observation (no prior
    // prev_slot_status_ entry) - first-observation is a baseline and never
    // fires.
    void check_hardware_event_clear(SlotInfo& slot, int slot_index, SlotStatus previous_status,
                                    SlotStatus current_status,
                                    const helix::ams::SpoolEvidence& inserted);

    /// Classify one insert against the reading the bay's previous occupant
    /// left in last_spool_evidence_ and act on the verdict:
    /// DifferentSpool clears the override, NoEvidence offers the same-spool
    /// notice, SameSpool keeps everything. Caller holds mutex_.
    void judge_insert_locked(SlotInfo& slot, int slot_index,
                             const helix::ams::SpoolEvidence& inserted);

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

    /// Set once a manager object has stated `current_index` (object path or the
    /// REST bridge's `ace_manager`). That driver owns the seat, so a G-code ack
    /// must not stamp one: it answers a toolchange it will not perform with a
    /// plain respond_info and no error, and `current_index` then stays at -1 —
    /// unchanged, so Klipper sends no frame to contradict the stamp
    /// (prestonbrown/helixscreen#1676).
    bool manager_states_seat_ = false;

    /// Whether the driver has published `ace_pro_enabled`, and its last value.
    /// Absent means this rig has no master switch at all, which is a different
    /// answer from "has one, currently on".
    bool ace_pro_enabled_seen_ = false;
    bool ace_pro_enabled_ = true;

    /// Macros that turn the ACE path off (engaging bypass) and back on.
    std::string bypass_on_macro_;
    std::string bypass_off_macro_;

    /// The two path sensors, when the driver publishes them. An unloaded strand
    /// parks just short of the hub rather than back at the spool, so these are
    /// what say where filament actually sits between slot and nozzle
    /// (prestonbrown/helixscreen#1678). A hub reporting neither leaves the path
    /// answering from the seat alone.
    bool path_sensors_seen_ = false;
    bool rdm_sensor_ = false;
    bool toolhead_sensor_ = false;

    // Shared helper used by every override-clear path (hardware event and
    // explicit user request). Caller must hold mutex_. Erases
    // overrides_[slot_index], resets override-exclusive fields on the
    // provided SlotInfo (brand, spool_name, spoolman_*, weights, color_name),
    // and fires clear_async. Color/material stay untouched — firmware owns
    // them for ACE and the parse has just refreshed them.
    void clear_override_locked(int slot_index, SlotInfo& slot);

    // User-provided per-slot metadata (brand, spool name, spoolman IDs,
    // remaining weight, etc.) layered over firmware-reported state.
    // Both writers (on_started initial load, apply_user_edit) hold
    // mutex_; so do the readers (apply_user_edit's re-read of the staged record,
    // clear_override_locked).
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> override_store_;
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides_;

    /// The shared lane_data namespace this backend co-authors. request_resync()
    /// re-reads it only where firmware states no identity of its own.
    helix::ams::FilamentSlotOverrideStore* lane_record_store() override {
        return override_store_.get();
    }

    // Previous slot status per slot index. Used as the insert-edge signal:
    // an EMPTY -> present transition runs the insert rule. Map presence also
    // acts as the baseline guard: absent entry means "no prior observation"
    // and the check is skipped (first observation never clears). Access is
    // always under mutex_ (parse_ace_object is the only writer/reader);
    // cleared in on_started() so a reconnect re-baselines.
    std::unordered_map<int, SlotStatus> prev_slot_status_;

    // What the hub's tag reader last got off the spool occupying each bay.
    // Written only by frames that carry a read (rfid true), and deliberately
    // NOT cleared when the bay empties: the reading of the spool that left is
    // the comparison side of the insert rule when the next one arrives, and a
    // no-read frame stating hub memory must not overwrite it. All access
    // under mutex_ (parse_ace_object); cleared in on_started() so a
    // reconnect re-baselines.
    std::unordered_map<int, helix::ams::SpoolEvidence> last_spool_evidence_;

    // Inserts whose tag read had not landed when the bay reported the spool:
    // slot index -> parse passes since the arm. Armed by
    // check_hardware_event_clear on the EMPTY -> present edge of a frame
    // carrying no read; judged and erased on the first rfid-bearing frame for
    // the bay; erased when the bay empties or when the read expires. All
    // access under mutex_ (parse_ace_object).
    std::unordered_map<int, int> pending_insert_reads_;
};

} // namespace helix

#endif // HELIX_HAS_ACE
