// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"

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
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;

    [[nodiscard]] PathTopology get_topology() const override;
    [[nodiscard]] PathTopology get_unit_topology(int unit_index) const override;
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    /// `units[].slots[].loaded` is reported per slot.
    [[nodiscard]] bool has_per_slot_loaded_authority() const override {
        return true;
    }

    /// Unload is offered only where the manager advertises a command for it.
    [[nodiscard]] bool can_unload_from_toolhead(int slot_index) const override;

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
    AmsError set_tool_mapping_impl(int tool_number, int slot_index) override;
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;
    [[nodiscard]] bool owns_tool_mapping_table() const override {
        return true;
    }

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
    void handle_status_update(const nlohmann::json& notification) override;
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

    /// One `groups[]` entry, with its members as global slot indices.
    struct Group {
        std::string name;
        std::string lane;
        std::vector<int> slots;
    };

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
    [[nodiscard]] AmsError load_gcode_locked(int slot_index, std::string& gcode) const;
    [[nodiscard]] bool slot_loadable_locked(int slot_index) const;
    [[nodiscard]] int loaded_lane_count_locked() const;
    [[nodiscard]] static AmsAction action_from_lane_state(const std::string& state);

    /// Last full view of oams_manager: status updates carry only the fields
    /// that changed, and are merged over this.
    nlohmann::json snapshot_ = nlohmann::json::object();
    bool api_supported_ = false;
    bool manager_ready_ = false;
    PathTopology topology_ = PathTopology::HUB;
    AmsAction reported_action_ = AmsAction::IDLE;
    std::vector<std::string> lane_states_;
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

    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> override_store_;
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides_;
};

} // namespace helix
