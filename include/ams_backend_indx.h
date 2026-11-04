// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bondtech INDX: up to ten passive nozzles parked in a dock, picked up one at a
// time by a Smart Head that carries the only extruder and the only (induction)
// heater. A tool changer to the UI: one slot per tool, each with its own
// filament and spool, a change is a swap.
//
// Built on AmsBackendToolChanger, which already owns the tool slots, the slot
// store, the optimistic swap dispatch and park. INDX publishes no changer
// status object, so the tool on the head comes from the macro contract in
// indx_contract.h, fed to the base as a tool reading.

#include "ams_backend_toolchanger.h"
#include "axis.h"
#include "indx_contract.h"
#include "tool_offsets.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace helix {

class AmsBackendIndx : public AmsBackendToolChanger {
  public:
    AmsBackendIndx(IMoonrakerAPI* api, helix::IMoonrakerClient* client);

    void set_discovery(const helix::PrinterDiscovery& discovery) override;

    [[nodiscard]] AmsType get_type() const override {
        return AmsType::INDX;
    }

    /// Status objects this backend reads; empty for any other printer.
    [[nodiscard]] static nlohmann::json required_status_objects(const helix::PrinterDiscovery& hw);

    static constexpr BackendTraits kIndxTraits = [] {
        BackendTraits t = kTraits;
        // LOAD_FILAMENT picks the tool up, heats it and feeds: the UI neither
        // preheats nor needs the head loaded first.
        t.load_feeds_filament = true;
        t.supports_auto_heat_on_load = true;
        t.heads_share_heater = true;
        return t;
    }();
    [[nodiscard]] BackendTraits traits() const override {
        return kIndxTraits;
    }

    /// True when the printer's own T<n> or CHANGE_TOOL macros switch the
    /// Spoolman active spool; HelixScreen then posts nothing.
    [[nodiscard]] bool manages_active_spool() const override {
        return macros_set_active_spool_;
    }

    /// The one extruder never changes; the mounted tool is save_variables'
    /// active_tool, and tool N is slot N.
    [[nodiscard]] std::vector<FeedSlotSignal> feed_slot_signals() const override;

    /// Every tool prints through the one extruder, so per-extruder usage says
    /// nothing about which tool used it; the tracker charges the mounted slot.
    [[nodiscard]] std::optional<int> slot_for_extruder(int /*extruder_idx*/) const override {
        return std::nullopt;
    }

    [[nodiscard]] std::string nozzle_heat_blocked_reason() const override;

    /// The macro homes; HelixScreen sends no G28 of its own.
    [[nodiscard]] bool delegates_homing_to_printer() const override {
        return true;
    }

    [[nodiscard]] HeadFilamentOps head_filament_ops(int head) const override;
    [[nodiscard]] bool unload_is_manual() const override {
        return true;
    }
    /// Every tool in one Load: one LOAD_FILAMENT per tool, in order. Each
    /// leaves its tool mounted and hot, so the last one stays on the head.
    [[nodiscard]] int max_heads_per_filament_op() const override;

    [[nodiscard]] std::optional<helix::ErrorEvent> current_error() const override;

    /// Seat or remove the half-seated tool, each behind a confirm. Empty
    /// without the MANUAL_* macros or a seat fault. Caller holds mutex_.
    [[nodiscard]] std::vector<helix::RecoveryAction> build_recovery_actions() const override;

    /// Why a change or park would be refused now, or empty. The same answer
    /// do_change_tool() and do_park_active_tool() refuse with.
    [[nodiscard]] std::string motion_blocked_reason() const;

    /// Speed, Calibration, Maintenance and Tool Offsets, each only when it
    /// holds an action, after the base's own.
    [[nodiscard]] std::vector<helix::printer::DeviceSection> get_device_sections() const override;
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;

    /// After a material edit, offer to update the heater model: only for the
    /// mounted tool or an empty head, never while printing, never for a tool
    /// without a hotend, never when the preset did not change.
    [[nodiscard]] std::optional<EditFollowUp> edit_follow_up(int slot_index, const SlotInfo& before,
                                                             const SlotInfo& after) const override;

  protected:
    /// A print is paused. Virtual so tests can pause without a printer.
    [[nodiscard]] virtual bool print_paused() const;

    AmsError additional_start_checks() override;
    void handle_status(const nlohmann::json& status) override;
    const char* backend_log_tag() const override {
        return "[AMS INDX]";
    }
    AmsError do_change_tool(int tool_number) override;
    AmsError do_park_active_tool() override;
    AmsError do_load_filament(int slot_index) override;
    /// Load only: unload is by hand. One ineligible tool refuses the batch.
    AmsError do_filament_batch(const std::vector<int>& slots, bool load) override;
    AmsError do_execute_device_action(const std::string& action_id, const std::any& value) override;
    /// The tool cycle and the calibrations report when the printer finishes
    /// them; every other action is the untracked one.
    AmsError do_run_device_action(const std::string& action_id, const std::any& value,
                                  DeviceActionDone on_done) override;

    AmsError do_run_edit_follow_up(int slot_index, const SlotInfo& before,
                                   const SlotInfo& after) override;

    /// Why the heater-model update for this edit may not be offered or sent
    /// now, or success with @p values set to the preset it sends: the
    /// SET_TOOL_FILAMENT macro, a preset the material names and the old one did
    /// not, a tool with a hotend, and that tool mounted or the head empty.
    /// Caller holds mutex_.
    [[nodiscard]] AmsError follow_up_refusal_locked(int slot_index, const SlotInfo& before,
                                                    const SlotInfo& after, std::string& preset_key,
                                                    indx::FilamentPreset& values) const;

    /// The nozzle heater's target in C; 0 without a printer. Virtual so tests
    /// can heat without one.
    [[nodiscard]] virtual double nozzle_target_c() const;

    /// head_filament_ops() with mutex_ held by the caller.
    [[nodiscard]] HeadFilamentOps head_filament_ops_locked(int head) const;

    /// Why @p slot may not be loaded now, or success: the checks a single
    /// load and every tool of a batch share. Caller holds mutex_.
    [[nodiscard]] AmsError load_refusal_locked(int slot) const;

    /// Raise the half-seated fault as the backend's ERROR action while
    /// seat_suspect holds, and withdraw it when it clears, so the error bridge
    /// presents current_error() and its recovery buttons. True when the action
    /// changed. Caller holds mutex_.
    bool apply_seat_fault_locked();

    /// Fold the offsets @p status carries into the held ones; true when one
    /// changed. Caller holds mutex_.
    bool merge_offsets_locked(const nlohmann::json& status);

    /// Size the tool slots to @p count, keeping the unit's INDX identity.
    void size_tools(int count);

    helix::indx::Contract contract_;
    helix::indx::Reading state_;
    /// The change command per tool index, from discovery.
    std::vector<std::string> change_gcodes_;
    bool macros_set_active_spool_ = false;
    int sized_tool_count_ = -1;
    /// An out-of-range active_tool was logged; reset by an in-range one.
    bool warned_out_of_range_ = false;

    /// apply_seat_fault_locked() holds the ERROR action. Under mutex_.
    bool seat_fault_raised_ = false;

    /// The printer's measured offsets, read-only. Under mutex_.
    helix::tool_offsets::OffsetReader offset_reader_;
    /// Whether discovery found offsets to show at all.
    bool offsets_readable_ = false;
    std::map<std::pair<int, Axis>, int> tool_offsets_um_;
    std::optional<int> global_z_offset_um_;
};

} // namespace helix
