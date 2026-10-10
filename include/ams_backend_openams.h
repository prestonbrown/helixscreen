// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace helix {

class OpenAmsTestAccess;

/// klipper_openams, read through the versioned `oams_manager` status contract
/// (include/openams_api.h). Nothing here reads AFC objects or sends AFC
/// commands: a printer with an `AFC` object stays with the AFC backend.
///
/// OpenAMS units are flattened into HelixScreen's global slot indices; each
/// slot keeps the manager's slot id, group and lane for dispatch. Every action
/// goes through the command the manager advertises for it, and an action whose
/// command is not advertised is refused on its own while the rest keep working.
class AmsBackendOpenAms : public AmsSubscriptionBackend {
  public:
    AmsBackendOpenAms(IMoonrakerAPI* api, helix::IMoonrakerClient* client);
    ~AmsBackendOpenAms() override = default;

    /// The oams_manager fields this backend reads, as a
    /// `printer.objects.subscribe` objects map; empty unless @p hw was claimed
    /// for OpenAMS. The nested lanes/units/groups arrays arrive whole on every
    /// change, so a topology change and the state that goes with it never land
    /// half-applied.
    [[nodiscard]] static nlohmann::json required_status_objects(const helix::PrinterDiscovery& hw);

    [[nodiscard]] AmsType get_type() const override {
        return AmsType::OPENAMS;
    }
    [[nodiscard]] AmsSystemInfo get_system_info() const override;

    [[nodiscard]] PathTopology get_topology() const override;
    [[nodiscard]] PathTopology get_unit_topology(int unit_index) const override;
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    /// Constant capability answers; see BackendTraits.
    static constexpr BackendTraits kTraits = [] {
        BackendTraits t;
        // `units[].slots[].loaded` is reported per slot.
        t.has_per_slot_loaded_authority = true;
        return t;
    }();
    [[nodiscard]] BackendTraits traits() const override {
        BackendTraits t = kTraits;
        // The unit views show their climate readout only where a unit reports
        // one or offers a dryer (openams only, never klipper_openams).
        t.has_environment_sensors = has_unit_climate_;
        return t;
    }

    /// Unload is offered only where the manager advertises a command for it.
    [[nodiscard]] bool can_unload_from_toolhead(int slot_index) const override;
    [[nodiscard]] std::optional<helix::ErrorEvent> current_error() const override;

    /// A load needs the lane cleared only when another slot on the TARGET's
    /// lane is loaded; slots on other lanes share nothing with it.
    [[nodiscard]] bool needs_unload_before_load(const AmsSystemInfo& info,
                                                int target_slot) const override;

    AmsError recover() override;
    AmsError reset() override;

    /// Only a load the manager is running on its own (a runout reload) can be
    /// cancelled. A load started here holds Klipper's G-code queue until it
    /// finishes, so the cancel command could not run until there is nothing
    /// left to cancel; that case is refused as busy.
    AmsError cancel() override;
    [[nodiscard]] bool can_cancel_operation() const override;

    /// Bookkeeping only: drops the failure a dispatch latched. The manager's
    /// own errors are cleared by reset().
    AmsError clear_fault(int slot_index) override;

    AmsError apply_user_edit(int slot_index, const SlotInfo& info,
                             const helix::ams::Observation& declared) override;
    AmsError sync_external_identity(int slot_index, const SlotInfo& info) override;
    void persist_slot_weight(int slot_index, float remaining_weight_g,
                             float total_weight_g) override;
    void persist_external_identity_impl(int slot_index,
                                        const helix::ams::Observation& spoolman) override;
    void clear_slot_override(int slot_index) override;

    /// Group names are configured in klipper_openams; `T<n>` groups are read
    /// as tool n and cannot be re-aimed from here.
    [[nodiscard]] AmsError can_set_tool_mapping(int tool_number, int slot_index) const override;
    AmsError set_tool_mapping_impl(int tool_number, int slot_index) override;
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;
    [[nodiscard]] bool owns_tool_mapping_table() const override {
        return true;
    }

    /// Each unit's own temperature and humidity ride AmsUnit::environment; the
    /// dryer is the unit's own heater (openams only: klipper_openams publishes no
    /// `devices`, so neither exists there). A unit offers it when it reports the
    /// dryer capability and advertises both dryer_start and dryer_stop. Start
    /// clamps to the unit's published range and sends
    /// `OAMS_DRYER_START OAMS=<idx> TARGET=<C> DURATION=<s>`.
    [[nodiscard]] DryerInfo get_dryer_info(int unit = 0) const override;
    AmsError start_drying(float temp_c, int duration_min, int fan_pct = -1, int unit = 0) override;
    AmsError stop_drying(int unit = 0) override;

    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override {
        return false;
    }

  protected:
    AmsError do_load_filament(int slot_index) override;
    AmsError do_unload_filament(int slot_index) override;
    AmsError do_select_slot(int slot_index) override;
    AmsError do_change_tool(int tool_number) override;

    void on_started() override;
    void on_stopping() override;
    void handle_status(const nlohmann::json& status) override;
    const char* backend_log_tag() const override {
        return "[AMS OpenAMS]";
    }
    SlotInfo* cached_slot_locked(int slot_index) override;
    void prepare_lane_repaint_locked(int slot_index, SlotInfo& slot) override;

    /// OpenAMS reports no filament identity, so the persisted lane record is
    /// the only account of what a slot holds and a resync re-reads it.
    [[nodiscard]] bool firmware_publishes_lane_identity() const override {
        return false;
    }
    helix::ams::FilamentSlotOverrideStore* lane_record_store() override {
        return override_store_.get();
    }

    /// Sends one advertised load/unload command. The command runs the complete
    /// configured toolchange, homing included, so no home is added here; the
    /// failure it raises reaches the user through the G-code error stream,
    /// which is why @p on_error only unwinds. A test seam.
    virtual AmsError send_operation_gcode(const std::string& gcode,
                                          std::function<void()> on_complete,
                                          std::function<void(const MoonrakerError&)> on_error);

  private:
    friend class OpenAmsTestAccess;

    /// One entry of a unit's published fault list.
    struct UnitFault {
        std::string severity; ///< "stop", "pause", ...
        std::string code;
        std::string text;
        int bay = -1;           ///< unit-local bay, -1 for the whole unit
        bool clearable = false; ///< the unit advertises clear_fault for it
    };

    /// One `groups[]` entry, with its members as global slot indices.
    struct Group {
        std::string name;
        std::string lane;
        std::vector<int> slots;
    };

    /// Re-read the lane_data records and file what they say onto the lanes.
    /// The read never blocks and never holds the backend mutex; the filing
    /// runs on the main thread. A spool link written after start (by the
    /// openams_spoolman component, or by another tool) shows up through this.
    void refresh_lane_records();
    void apply_lane_records(int backend_block,
                            const std::unordered_map<int, helix::ams::LaneDataRecord>& records);

    /// What a unit publishes about its dryer. Parallel to system_info_.units.
    struct UnitDryer {
        bool offered = false;           ///< capability plus dryer_start or dryer_stop
        bool can_start = false;         ///< dryer_start is advertised now
        bool can_stop = false;          ///< dryer_stop is advertised now
        bool requires_unloaded = false; ///< no bay may be loaded while it runs
        std::optional<EnvironmentData> environment;
        DryerInfo info;
    };

    /// Parallel to system_info_.units.
    std::vector<std::vector<UnitFault>> unit_faults_;
    std::vector<UnitDryer> unit_dryers_;
    std::atomic<bool> has_unit_climate_{false};
    /// Length of the cycle this screen started, per unit index: the unit
    /// reports only the time left.
    std::unordered_map<int, int> requested_dry_min_;
    std::vector<int> unit_oams_idx_; ///< `units[].id` as a number, -1 when it is not one

    /// The gcode that clears everything the manager and the units hold: a
    /// firmware fault clear for each unit that offers one, then the manager's
    /// own reset. Empty when there is nothing to send. Caller holds mutex_.
    [[nodiscard]] std::string clear_script_locked() const;
    [[nodiscard]] bool has_clearable_fault_locked() const;
    [[nodiscard]] std::string fault_detail_locked() const;

    void parse_snapshot_locked();
    void present_nothing_locked();
    AmsError begin_operation(AmsAction action, int slot_index, const std::string& gcode,
                             const char* completion_event);
    void finish_operation(std::uint64_t generation, const char* event);
    void fail_operation(std::uint64_t generation, const MoonrakerError& error);

    /// Refusal for any action while the manager cannot take one, else success.
    [[nodiscard]] AmsError manager_accepts_locked() const;
    /// The advertised command for @p action ("load", "unload", "cancel",
    /// "reset"), or empty when the manager does not offer it.
    [[nodiscard]] std::string command_locked(const char* action) const;
    /// The slot loaded on @p lane, or -1.
    [[nodiscard]] int loaded_slot_on_lane_locked(const std::string& lane) const;
    /// The load for @p slot_index. When another slot on the same lane is
    /// loaded, the lane is unloaded first in the same script, so the manager
    /// sees one operation.
    [[nodiscard]] AmsError load_gcode_locked(int slot_index, std::string& gcode) const;
    [[nodiscard]] bool slot_loadable_locked(int slot_index) const;
    [[nodiscard]] int loaded_lane_count_locked() const;
    /// The lane id the unload command names for @p slot_index, or empty when
    /// that slot is not the one loaded on any lane.
    [[nodiscard]] std::string loaded_lane_of_slot_locked(int slot_index) const;
    [[nodiscard]] static AmsAction action_from_lane_state(const std::string& state);

    /// Last full view of oams_manager: status updates carry only the fields
    /// that changed, and are merged over this.
    nlohmann::json snapshot_ = nlohmann::json::object();
    bool api_supported_ = false;
    bool manager_ready_ = false;
    PathTopology topology_ = PathTopology::HUB;
    AmsAction reported_action_ = AmsAction::IDLE;
    std::vector<std::string> lane_states_;
    /// Per lane (parallel to lane_states_): its id and the global slot it holds.
    std::vector<std::string> lane_ids_;
    std::vector<int> lane_loaded_slots_;
    /// The lane each global slot's unit feeds; empty when the unit names none.
    std::vector<std::string> slot_lanes_;
    /// Whether each manager slot id held a spool on the last frame; the
    /// baseline an insert is judged against.
    std::unordered_map<int, bool> present_by_slot_id_;

    /// A load or unload this backend dispatched and has not heard back from.
    AmsAction pending_action_ = AmsAction::IDLE;
    int pending_slot_ = -1;
    std::uint64_t operation_generation_ = 0;
    /// What the last failed dispatch reported, shown until the next action or
    /// clear_fault().
    std::string failure_detail_;

    // Global slot index -> manager slot id / group name.
    std::vector<int> remote_slot_ids_;
    std::vector<std::string> slot_groups_;
    std::vector<Group> groups_;
    std::unordered_map<std::string, std::string> commands_;

    /// OpenAMS states no identity of its own, so a clear blanks every
    /// identity field: nothing will restate them.
    void clear_override_fields(SlotInfo& slot) const override;
};

} // namespace helix
