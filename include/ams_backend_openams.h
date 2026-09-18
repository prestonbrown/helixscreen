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

/// Native OpenAMS backend.
///
/// This consumes only the versioned `oams_manager` status contract. It does
/// not inspect AFC objects, use AFC commands, or translate OpenAMS into AFC's
/// schema. OpenAMS units are flattened into HelixScreen's global slot indices
/// while their lane/unit relationship remains available on AmsUnit.
class AmsBackendOpenAms : public AmsSubscriptionBackend {
  public:
    static constexpr int SUPPORTED_API_VERSION = 1;

    AmsBackendOpenAms(IMoonrakerAPI* api, helix::IMoonrakerClient* client);
    ~AmsBackendOpenAms() override = default;

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

    [[nodiscard]] bool has_per_slot_loaded_authority() const override {
        return true;
    }

  protected:
    AmsError do_load_filament(int slot_index) override;
    AmsError do_unload_filament(int slot_index) override;
    AmsError do_select_slot(int slot_index) override;
    AmsError do_change_tool(int tool_number) override;

  public:
    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;

    AmsError apply_user_edit(int slot_index, const SlotInfo& info,
                             const helix::ams::Observation& declared) override;
    AmsError sync_external_identity(int slot_index, const SlotInfo& info) override;
    AmsError set_tool_mapping_impl(int tool_number, int slot_index) override;

    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override {
        return false;
    }

  protected:
    void on_started() override;
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override {
        return "[AMS OpenAMS]";
    }
    SlotInfo* cached_slot_locked(int slot_index) override;

    /// Test seam around the shared dispatch policy. Production uses the base
    /// helper with explicit log-only error ownership and skips app-side homing
    /// because the advertised OpenAMS macro owns the complete sequence.
    virtual AmsError send_operation_gcode(const std::string& gcode,
                                          std::function<void()> on_complete,
                                          std::function<void(const MoonrakerError&)> on_error);

  private:
    bool parse_snapshot_locked();
    void finish_operation(std::uint64_t generation, const char* event);
    void fail_operation(std::uint64_t generation, const MoonrakerError& error);
    AmsError begin_operation(AmsAction action, int slot_index, const std::string& gcode,
                             const char* completion_event);
    AmsError dispatch_simple_command(const std::string& command, const char* feature);

    [[nodiscard]] bool valid_slot_locked(int slot_index) const;
    [[nodiscard]] int slot_for_tool_locked(int tool_number) const;
    [[nodiscard]] static AmsAction action_from_lane_state(const std::string& state);
    [[nodiscard]] static PathTopology topology_for_kind(const std::string& kind);
    [[nodiscard]] static std::optional<PathTopology>
    topology_from_token(const std::string& topology);

    nlohmann::json snapshot_ = nlohmann::json::object();
    bool schema_supported_ = false;
    bool manager_ready_ = false;
    PathTopology topology_ = PathTopology::HUB;
    AmsAction reported_action_ = AmsAction::IDLE;
    AmsAction pending_action_ = AmsAction::IDLE;
    int pending_slot_ = -1;
    std::uint64_t operation_generation_ = 0;

    // Helix global slot index -> OpenAMS API slot id / group / lane.
    std::vector<int> remote_slot_ids_;
    std::vector<std::string> slot_groups_;
    std::vector<std::string> slot_lanes_;

    std::unordered_map<std::string, std::string> commands_;

    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> override_store_;
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides_;
};

} // namespace helix
