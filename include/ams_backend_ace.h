// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#if HELIX_HAS_ACE

#include "ace_slot_parse.h"
#include "ace_status_parse.h"
#include "ams_bypass_policy.h"
#include "ams_subscription_backend.h"
#include "async_lifetime_guard.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_binding.h"
#include "moonraker_types.h"
#include "vendor_state_push.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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

    /// The status objects ACE pushes, as a `printer.objects.subscribe` objects map:
    /// every detected ACE object name (the community drivers, native GoKlipper
    /// `filament_hub`, or the ACEPRO driver's `ace_instance_N`), falling back to `ace`.
    /// Empty unless @p hw detected ACE.
    [[nodiscard]] static nlohmann::json required_status_objects(const helix::PrinterDiscovery& hw);

    // ========================================================================
    // Type
    // ========================================================================

    [[nodiscard]] AmsType get_type() const override {
        return AmsType::ACE;
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

    // ========================================================================
    // Path Visualization
    // ========================================================================

    [[nodiscard]] PathTopology get_topology() const override;
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    /// Constant capability answers; see BackendTraits.
    static constexpr BackendTraits kTraits = [] {
        BackendTraits t;
        // Every parse path that resolves the seated slot stamps
        // SlotStatus::LOADED on it (see apply_seated_slot_stamp_locked), so the
        // per-slot status answers the per-slot question (#1199).
        t.has_per_slot_loaded_authority = true;
        t.has_environment_sensors = true;
        return t;
    }();
    [[nodiscard]] BackendTraits traits() const override {
        return kTraits;
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
    /// Also forgets the toolchange target the driver last stated, so the next
    /// frame cannot re-raise the toolchange the restart already ended.
    void on_klipper_restarted() override;

    // ========================================================================
    // Configuration
    // ========================================================================

    AmsError apply_user_edit(int slot_index, const SlotInfo& info,
                             const helix::ams::Observation& declared) override;
    AmsError sync_external_identity(int slot_index, const SlotInfo& info) override;

    /// A bay's last identified tag read (rfid true, or 2): material, colour and
    /// the temperatures the frame carried. Dropped when the bay empties or a
    /// finished read finds no tag.
    [[nodiscard]] std::optional<helix::ams::TagReading> tag_reading(int slot_index) const override;
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
    [[nodiscard]] AmsError can_set_tool_mapping(int tool_number, int slot_index) const override;
    AmsError set_tool_mapping_impl(int tool_number, int slot_index) override;

    // Nothing here can re-aim a tool: the backend declares RemapStrategy::None
    // and owns no editable tool->slot table.
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    /// T<n> loads slot_for_tool_locked(n): the slot whose ACEPRO tool number is
    /// n, or slot n on a driver that publishes no tool numbers.
    [[nodiscard]] helix::FirmwareRouting firmware_default_routing() const override;

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
    void set_bypass_macros(helix::BypassMacros macros);

    /// The bypass macros and the ACEPRO driver's `ace_instance_N` objects, from discovery.
    void set_discovery(const helix::PrinterDiscovery& discovery) override;

    // ========================================================================
    // Environment Sensors & Dryer Control (ACE Pro has built-in dryer + temp)
    // ========================================================================

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
    AmsError do_execute_device_action(const std::string& action_id, const std::any& value) override;

    // ========================================================================
    // Endless Spool
    // ========================================================================

    /// Available only once the driver has published `endless_spool_enabled`.
    /// Read-only: the driver derives its swap target from the match mode and
    /// the inventory, so there is no per-slot backup to write.
    [[nodiscard]] helix::printer::EndlessSpoolCapabilities
    get_endless_spool_capabilities() const override;

    /// The groups the current match mode induces over the ready slots.
    [[nodiscard]] helix::printer::EndlessSpoolConfig get_endless_spool_config() const override;

  protected:
    /// ACE_ENABLE_ENDLESS_SPOOL / ACE_DISABLE_ENDLESS_SPOOL.
    AmsError apply_endless_spool_enabled(bool enabled) override;

    // ========================================================================
    // AmsSubscriptionBackend hooks
    // ========================================================================

    void handle_status(const nlohmann::json& status) override;
    const char* backend_log_tag() const override {
        return "[ACE]";
    }
    void on_started() override;
    void on_stopping() override;

    // ========================================================================
    // Response handling (protected for unit testing): each reads its payload
    // through helix::ace (ace_status_parse.h) and applies it under mutex_.
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
     * @param unit The unit the object describes: N for the ACEPRO driver's
     *        `ace_instance_N`; kNotAnInstance (unit 0) for the ACEPRO
     *        manager and every single-object driver
     */
    void parse_ace_object(const nlohmann::json& data, int unit = kNotAnInstance);

    /// parse_ace_object's unit for an object that is no `ace_instance_N`.
    static constexpr int kNotAnInstance = -1;

  private:
    friend class AceTestAccess;

    /// Apply one object's parsed delta to unit @p unit (see parse_ace_object).
    /// Caller holds mutex_.
    void apply_ace_object_locked(const helix::ace::ObjectDelta& d, int unit);

    /// Keep or drop bay @p slot_index's tag reading from one frame's slot
    /// @p entry (its tag read, material, colour and temperatures) and
    /// @p present. Caller holds mutex_.
    void note_tag_frame_locked(int slot_index, const helix::ace::SlotDelta& entry,
                               std::optional<bool> present);

    /// Lay the REST bridge's temp_min / temp_max onto @p slot, unless a user
    /// edit moved the slot's temperatures this session. Caller holds mutex_.
    void apply_hub_temps_locked(int slot_index, SlotInfo& slot,
                                const helix::ace::SlotDelta& entry) const;

    /// Clear user_temps_ for @p slot_index. Caller holds mutex_.
    void forget_user_temps_locked(int slot_index);

    /// Per bay, whether a user edit this session set the temperatures; cleared
    /// with the override and at on_started(). Under mutex_.
    std::vector<bool> user_temps_;

    /// Per bay, the last identified tag read. Under mutex_.
    std::vector<std::optional<helix::ams::TagReading>> tag_readings_;

    // ========================================================================
    // REST Fallback (for BunnyACE/DuckACE without get_status())
    // ========================================================================

    void start_rest_fallback();
    void stop_rest_fallback();
    void rest_polling_loop();

    void poll_info();
    void poll_status();
    void poll_slots();

    /// What a poll does with an answered /status or /slots result, and what
    /// the plugin's pushed result gets: parse it and announce a change.
    /// Main thread.
    void apply_rest_status(const nlohmann::json& result);
    void apply_rest_slots(const nlohmann::json& result);

    /// What a poll does with a /status read that returned no data, and what
    /// a pushed error answer gets: count it, and surface a bridge that never
    /// answered. Main thread.
    void note_rest_status_failure(const std::string& error);

    /// Whether the REST thread polls /status and /slots on this pass: every
    /// pass while polling, every kVendorPushBackstop while the plugin pushes.
    static bool rest_data_poll_due(bool pushing, std::chrono::steady_clock::duration since_last);

    /// The bridge's /status and /slots answers pushed by the HelixPrint
    /// plugin. Main thread.
    void on_vendor_state(const nlohmann::json& payload);

    /// Fold the plugin's current answer into vendor_push_, polling the bridge
    /// once when that owes a read. Main thread.
    void refresh_vendor_push();

    /**
     * @brief Pick the ace/filament_hub/ace_instance_N object to parse from a
     *        status frame, by one preference order.
     *
     * Both selection passes share this so the key list cannot drift between
     * them. The predicate varies by caller: with @p require_slots the object
     * must carry a non-empty "slots" array — the on_started() query commits to
     * the WebSocket subscription path only on real slot data, so a manager-only
     * object (the ACEPRO driver's `ace` exposes `ace_instances`/`current_index`
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
     * @brief The lowest-numbered `ace_instance_N` object whose value satisfies
     *        @p predicate: the bootstrap's pick when it decides whether the
     *        ACEPRO driver's objects carry slot data (#1107). Walks
     *        ace_instance_objects, so the same instances in the same order.
     *
     * @param status A query result.status object or one notify frame's status
     * @param predicate Object filter; defaults to any instance object.
     *        Borrowed by select_ace_object with its own mode-specific filter.
     * @return The instance number and its object (borrowed from @p status),
     *         or nullopt
     */
    static std::optional<std::pair<int, const nlohmann::json*>> lowest_ace_instance(
        const nlohmann::json& status, const std::function<bool(const nlohmann::json&)>& predicate =
                                          [](const nlohmann::json&) { return true; });

    /**
     * @brief The manager-shaped `ace` object in a status frame, when one is
     *        present.
     *
     * The ACEPRO driver splits its surface: each unit's slots live on
     * `ace_instance_N`, while the top-level `ace` is a manager whose only
     * seat signal is `current_index` (#1069). A slot-bearing `ace` (ValgACE)
     * is NOT a manager — returning null for it keeps the caller from parsing
     * the same object twice. Any other non-empty `ace` is one: its notify
     * deltas omit `current_index` whenever the seat did not change.
     *
     * @param status A printer.objects.query result.status object, or one
     *        notify_status_update frame's status object
     * @return Pointer to the manager object (borrowed from @p status), or
     *         null when no manager-shaped `ace` is present
     */
    static const nlohmann::json* manager_ace_object(const nlohmann::json& status);

    /// `<verb>` addressed at global @p slot in the form the driver parses. The
    /// ACEPRO driver resolves a unit from INSTANCE= (the unit index, which is
    /// its instance number) and reads INDEX= as that unit's local slot,
    /// rejecting INDEX= alone; GoKlipper and ValgACE drive a single hub by
    /// INDEX= only.
    std::string slot_command(const std::string& verb, int slot) const;

    /// Every non-empty `ace_instance_N` object in a status frame with its
    /// instance number N below kMaxUnits, in ascending numeric N.
    static std::vector<std::pair<int, const nlohmann::json*>>
    ace_instance_objects(const nlohmann::json& status);

    /// The `ace_instance_N` key for instance @p n.
    static std::string ace_instance_key(int n);

    /// Fold the manager's path sensors (`rdm_sensor` at the hub,
    /// `toolhead_sensor` at the extruder) into the cached readings and onto
    /// every unit, which all feed that one hub. A frame stating neither leaves
    /// the last reading standing: notify frames carry only changed fields, so
    /// silence is not a cleared sensor. Caller holds mutex_.
    void apply_path_sensors_locked(const helix::ace::PathSensors& sensors);

    /// Seat the loaded tool from the ACEPRO manager's `current_index` — the
    /// driver's tool number, -1 = nothing loaded. A tool no known slot carries
    /// is stated with no slot marked (the stamp helper guards
    /// current_slot < 0). Caller holds mutex_.
    /// @return true when current_slot/current_tool/filament_loaded changed
    bool seat_from_tool_locked(int tool);

    /// The global slot that feeds driver tool @p tool, or -1 when no slot
    /// does. Read from each slot's mapped_tool, the tool number each ACEPRO
    /// slot publishes; a driver that publishes none numbers its tools by slot.
    /// Caller holds mutex_.
    int slot_for_tool_locked(int tool) const;

    /// The driver tool global slot @p slot_index feeds, the inverse of
    /// slot_for_tool_locked; -1 for an ACEPRO slot the driver has named no tool
    /// for. Caller holds mutex_.
    int tool_for_slot_locked(int slot_index) const;

    /// Unit @p unit, created when the frame names it first; units below it that
    /// have not reported yet hold kSlotsPerUnit slots, disconnected. Caller
    /// holds mutex_.
    AmsUnit& unit_locked(int unit);

    /// Publish every slot's mapped_tool as the tool map, in both directions;
    /// with no slot naming a tool the map is empty. Caller holds mutex_.
    void publish_tool_map_locked();

    /// Drop every unit at or past @p count, with its dryer, status word and
    /// link: the bootstrap answer names every unit there is, so one it does
    /// not name is gone (a restart onto fewer units, or onto another driver).
    /// Locks mutex_.
    void keep_units(size_t count);

    /// Number every unit's slots globally, unit after unit, and publish the
    /// total. Returns true when a unit that had slots moved. Caller holds mutex_.
    [[nodiscard]] bool index_units_locked();

    /// The ` INSTANCE=N` argument (with its leading space) dryer commands
    /// address unit @p unit by: empty on drivers with one unit, nullopt for a
    /// unit the ACEPRO driver does not have. Locks mutex_.
    [[nodiscard]] std::optional<std::string> dryer_instance_argument(int unit) const;

    /// The ACEPRO instance number unit @p unit is: the unit index itself when
    /// each `ace_instance_N` is its own unit, and on the REST bridge (which
    /// serves one unit) the instance its /status reported serving. Caller
    /// holds mutex_.
    [[nodiscard]] int ace_instance_for_unit_locked(int unit) const;

    /// Unit @p unit's dryer, created with the ACE Pro's defaults when the
    /// frame names it first. Caller holds mutex_.
    DryerInfo& dryer_locked(int unit);

    /// Apply what each unit says about itself (unit_identities_) to the
    /// units: its model as the unit's name, its firmware, and on the ACEPRO
    /// driver a display name, the short product name for its protocol,
    /// numbering the second and later unit of one name. Caller holds mutex_.
    void apply_unit_identities_locked();

    /// End a load or unload this backend sent, at its ack or error: a
    /// LOADING or UNLOADING reads IDLE, anything else (an ERROR a unit
    /// reported while it ran) is left standing. Caller holds mutex_.
    void end_own_op_locked();

    /// Record unit @p unit's own status word and publish the action of the
    /// first unit that is doing something, or IDLE. Caller holds mutex_.
    void set_unit_action_locked(int unit, AmsAction action);

    /// Track the ACEPRO manager's `target_index` (the global tool a toolchange
    /// is heading for, -1 when none is under way). While it names a tool
    /// that is not seated, the target is the pending slot and an idle hub
    /// reads as LOADING, so a toolchange the printer started itself (PRINT_START,
    /// a T<n> mid-print, a macro) shows on the path before the tool seats.
    /// Swapping out a seated tool reads as UNLOADING until the hub sensor
    /// clears, so the path stays on the outgoing lane while it retracts.
    /// Caller holds mutex_; parse the path sensors first.
    /// @return true when pending_target_slot or action changed
    bool apply_target_index_locked(std::optional<int> target_index);

    /// Seat the loaded tool from a slot index (loaded_slot, the ValgACE
    /// "loaded" scan, current_filament's parsed local index): -1 clears,
    /// otherwise the index is the slot and the tool. Caller holds mutex_.
    /// @return true when current_slot/current_tool/filament_loaded changed
    bool seat_from_local_index_locked(int slot_index);

    // ========================================================================
    // Members
    // ========================================================================

    // Each unit's dryer, by unit index (ACE-specific, not in base class).
    // Guarded by mutex_.
    std::vector<DryerInfo> dryers_;

    // Each unit's own status word as an action, by unit index. Guarded by
    // mutex_.
    std::vector<AmsAction> unit_actions_;

    // Each ACEPRO unit's link from its `connection_state`, by unit: true when
    // connected. A unit with no entry has not stated one. Guarded by mutex_.
    std::map<int, bool> unit_links_;

    // The unit-local slot each unit's driver is feed-assisting
    // (`feed_assist_slot`), -1 for none, by unit. A unit with no entry has not
    // stated one. Guarded by mutex_.
    std::map<int, int> feed_assist_slots_;

    /// The Feed Assist toggle for the loaded slot: on when its unit assists that
    /// slot, without a value until the unit has said, and disabled with nothing
    /// loaded.
    [[nodiscard]] helix::printer::DeviceAction feed_assist_action_locked() const;

    // What each unit states about itself, by unit index. Kept apart from the
    // units: a notify frame can name a unit before its slots arrive, and a
    // later frame restates only the fields that changed. Guarded by mutex_.
    struct UnitIdentity {
        std::string model;     // `model`, e.g. "Anycubic Color Engine Pro"
        std::string protocol;  // `protocol`: "ace1_json" (ACE Pro) or "ace2_proto" (ACE 2)
        std::string firmware;  // `firmware`
        bool reported = false; // the driver has published this instance
    };
    std::vector<UnitIdentity> unit_identities_;

    // The ACEPRO driver's `ace_instance_N` objects discovery found, which the bootstrap
    // queries. Guarded by mutex_.
    std::vector<std::string> discovered_instances_;

    // Set once the driver publishes per-unit `ace_instance_N` objects (the
    // ACEPRO driver), whose commands name a unit or a tool. Guarded by mutex_.
    bool instance_objects_ = false;

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

    // Set once the ACEPRO driver's surface is seen (`ace_instance_N` objects,
    // or the REST bridge's `ace_manager`): its slot commands take INSTANCE= +
    // INDEX=. Reset on every start. Guarded by mutex_.
    bool instance_commands_ = false;

    // The ACEPRO instance the REST bridge's /status serves (`instance_index`):
    // without ?instance= the bridge picks it from current_index, so it is read
    // back rather than assumed. Guarded by mutex_.
    int rest_instance_ = 0;

    /// Guards ACE's own RPC and REST callbacks. Separate from the base
    /// lifetime_ because cancel() and on_stopping() expire these without
    /// expiring the status subscription.
    helix::AsyncLifetimeGuard op_lifetime_;

    // REST fallback state
    bool use_rest_fallback_{false};
    // The HelixPrint plugin pushes the bridge's answers while its feature is
    // ready; the REST loop skips its data polls for as long as it does.
    helix::VendorStateTracker vendor_push_;
    static constexpr const char* kVendorPushFeature = "vendor_ace";
    static constexpr const char* kVendorPushVendor = "ace";
    static constexpr int kVendorPushMinVersion = 1;
    /// Passes on which the REST thread polled /status and /slots.
    std::atomic<int> rest_data_polls_{0};
    std::thread rest_polling_thread_;
    std::atomic<bool> rest_stop_requested_{false};
    std::condition_variable rest_stop_cv_;
    std::mutex rest_stop_mutex_;

    // Configuration
    static constexpr int kPollIntervalMs = 500;
    /// Consecutive data-endpoint (/status) failures before surfacing the
    /// "Moonraker bridge not found" error (genuinely-missing-bridge case).
    static constexpr int kMaxDataFetchFailures = 3;
    static constexpr int kSlotsPerUnit = helix::ace::kSlotsPerUnit;
    static constexpr int kMaxUnits = helix::ace::kMaxUnits;

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

    /// Mutable slot lookup by global index. Walks each unit's
    /// first_slot_global_index and its slots vector rather than
    /// AmsSystemInfo::get_slot_global(), which reads slot_count: the two REST
    /// parsers size units[0].slots independently, and parse_slots_response
    /// only refreshes slot_count when the slot count actually changes.
    /// Caller holds mutex_.
    SlotInfo* mutable_slot_locked(int slot_index);
    /// Reads the slots the same way, so a read sees what a write through
    /// mutable_slot_locked() left.
    [[nodiscard]] const SlotInfo* slot_info_locked(int slot_index) const override {
        return const_cast<AmsBackendAce*>(this)->mutable_slot_locked(slot_index);
    }

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
    ///
    /// Also publishes system_info_.filament_loaded from seat_loaded_, so the
    /// seat and the stamp cannot disagree. Returns whether that flag changed.
    bool apply_seated_slot_stamp_locked();

    /// The seat reaches the nozzle: what the path draws, not what the driver names.
    [[nodiscard]] bool seat_at_nozzle_locked() const;

    /// The driver names a seated tool, idle, but both path sensors read clear.
    [[nodiscard]] bool path_empty_under_seat_locked() const;

    /// The tool the driver states is seated, before the path sensors are
    /// weighed. system_info_.filament_loaded is the published answer, derived
    /// from this by apply_seated_slot_stamp_locked().
    bool seat_loaded_ = false;

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

    /// Last `target_index` the manager stated (-1 = no toolchange under way),
    /// whether the LOADING/UNLOADING currently shown was raised from it rather
    /// than by this backend's own load command, and whether the outgoing
    /// strand has cleared the hub during this toolchange. The hub sensor is
    /// made again once the incoming strand arrives, so only the latch tells
    /// the two phases apart.
    int target_index_ = -1;
    bool driver_action_ = false;
    // A load this backend sent is in flight: its LOADING ends with the
    // command's ack, not when the driver's target clears. Guarded by mutex_.
    bool own_load_ = false;
    bool swap_hub_cleared_ = false;

    /// Whether the driver has published `ace_pro_enabled`, and its last value.
    /// Absent means this rig has no master switch at all, which is a different
    /// answer from "has one, currently on".
    bool ace_pro_enabled_seen_ = false;
    bool ace_pro_enabled_ = true;

    [[nodiscard]] helix::printer::EndlessSpoolConfig endless_spool_config_locked() const;

    /// Slots whose raw driver status is exactly "ready": the only swap targets
    /// the driver's endless spool considers (our SlotStatus folds preload,
    /// running, loaded and available into the same value).
    std::set<int> driver_ready_slots_;
    void note_driver_ready_locked(int global_index, const std::string& driver_status);

    /// The driver's endless-spool switch and match mode ("exact" | "material" |
    /// "next"), from the manager object. Unseen means the driver predates it.
    bool endless_spool_seen_ = false;
    bool endless_spool_on_ = false;
    std::string endless_spool_mode_ = "exact";

    /// Macros that turn the ACE path off (engaging bypass) and back on.
    std::string bypass_on_macro_;
    std::string bypass_off_macro_;

    /// What discovery found for the pair, and the macros the user may pick
    /// instead (kAutoMacro first). Re-resolved when a pick changes.
    helix::BypassMacros bypass_detected_;
    std::vector<std::string> bypass_macro_options_;

    using ParkState = helix::ace::ParkState;

    /// Records slot `index`'s park_state. Returns whether it changed.
    bool note_park_state_locked(int index, ParkState state);

    /// An idle slot the driver parked draws staged at the hub: not the seated
    /// slot, no toolchange or load under way, filament present.
    [[nodiscard]] bool slot_parked_at_hub_locked(int slot_index) const;

    std::map<int, ParkState> park_states_;

    /// The two path sensors, when the driver publishes them. An unloaded strand
    /// parks just short of the hub rather than back at the spool, so these are
    /// what say where filament actually sits between slot and nozzle
    /// (prestonbrown/helixscreen#1678). A hub reporting neither leaves the path
    /// answering from the seat alone.
    bool path_sensors_seen_ = false;
    bool rdm_sensor_ = false;
    bool toolhead_sensor_ = false;

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
