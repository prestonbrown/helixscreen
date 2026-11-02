// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_indx.h"

#include "ams_device_action_value.h"
#include "ams_device_sections.h"
#include "ams_error.h"
#include "ams_fault_event.h"
#include "app_constants.h"
#include "display_numbering.h"
#include "filament_op_slot_resolver.h"
#include "i_moonraker_api.h"
#include "klipper_extruder_naming.h"
#include "lane_source_store.h"
#include "lvgl.h"
#include "print_lifecycle_state.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "toolchanger_addon.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <any>
#include <memory>
#include <utility>

namespace helix {

AmsBackendIndx::AmsBackendIndx(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
    : AmsBackendToolChanger(api, client) {
    system_info_.type = AmsType::INDX;
    system_info_.type_name = "INDX"; // i18n: do not translate - product name
}

nlohmann::json AmsBackendIndx::required_status_objects(const helix::PrinterDiscovery& hw) {
    return indx::required_status_objects(hw);
}

void AmsBackendIndx::set_discovery(const helix::PrinterDiscovery& discovery) {
    contract_ = indx::check_contract(discovery);
    macros_set_active_spool_ = indx::macros_set_active_spool(discovery);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        offset_reader_ = tool_offsets::resolve_reader(discovery);
        offsets_readable_ = tool_offsets::reads_axis(discovery, Axis::Z);
    }
    change_gcodes_.clear();
    for (int i = 0; i < indx::kMaxTools; ++i) {
        change_gcodes_.push_back(indx::change_tool_gcode(discovery, i));
    }

    // The swap is INDX's own T<n>/CHANGE_TOOL, never SELECT_TOOL: marking the
    // commands present gives the base the G-code-rewrite remap and park.
    toolchanger_addon::ToolCommands commands;
    commands.present = true;
    commands.provider_name = "INDX"; // i18n: do not translate - product name
    commands.select_prefix = "T";
    commands.unselect = indx::park_gcode();
    set_tool_commands(std::move(commands));

    if (!contract_.recognized) {
        spdlog::warn("{} INDX macros not recognized (missing: {}); nothing will be sent",
                     backend_log_tag(), indx::missing_text(contract_));
        size_tools(0);
        return;
    }
    spdlog::info("{} INDX macros recognized: {} tools from config, load={}, own spool macros={}",
                 backend_log_tag(), indx::config_tool_count(), contract_.has_load_filament,
                 macros_set_active_spool_);
    size_tools(indx::config_tool_count());
}

void AmsBackendIndx::size_tools(int count) {
    count = std::clamp(count, 0, indx::kMaxTools);
    if (count == sized_tool_count_) {
        return;
    }
    sized_tool_count_ = count;
    std::vector<std::string> names;
    for (int i = 0; i < count; ++i) {
        names.push_back(helix::ui::tool_label(i));
    }
    set_discovered_tools(std::move(names));
    std::lock_guard<std::mutex> lock(mutex_);
    if (!system_info_.units.empty()) {
        system_info_.units[0].name = "INDX"; // i18n: do not translate - product name
        // Every tool prints through the Smart Head's one extruder.
        for (auto& slot : system_info_.units[0].slots) {
            slot.extruder_name = helix::extruder_name_for_tool(0);
        }
    }
}

AmsError AmsBackendIndx::additional_start_checks() {
    // Starts with no tools on purpose: an unrecognized macro set renders its
    // error, and a config without variable_tool_count sizes on the first frame.
    return AmsErrorHelper::success();
}

void AmsBackendIndx::handle_status(const nlohmann::json& status) {
    auto update = indx::read_status(status);
    bool offsets_changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        offsets_changed = merge_offsets_locked(status);
    }
    if (!update) {
        if (offsets_changed) {
            emit_event(EVENT_STATE_CHANGED);
        }
        return;
    }

    std::optional<int> resize;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const indx::Reading before = state_;
        indx::merge(state_, *update);
        changed = !(state_ == before) || offsets_changed;
        changed |= apply_seat_fault_locked();
        if (contract_.recognized && update->tool_count &&
            *update->tool_count != sized_tool_count_) {
            resize = *update->tool_count;
        }
    }
    if (!changed && !resize) {
        // Moonraker resends unchanged macro state; nothing on screen moves.
        return;
    }
    if (resize) {
        size_tools(*resize);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.active_tool) {
            const int tool = *state_.active_tool;
            if (tool < -1 || tool >= sized_tool_count_) {
                // A tool the printer has not configured: not a reading to act on.
                if (!warned_out_of_range_) {
                    spdlog::warn("{} active_tool {} is outside the {} configured tools; "
                                 "treating the mounted tool as unknown",
                                 backend_log_tag(), tool, sized_tool_count_);
                    warned_out_of_range_ = true;
                }
            } else {
                warned_out_of_range_ = false;
                toolchanger_addon::ToolReading reading;
                reading.current_tool = tool;
                apply_tool_sensor_locked(reading);
            }
        }

        // T<n>.spool_id is a spool the printer's own macros track per tool.
        for (const auto& [tool, id] : update->tool_spool_ids) {
            if (tool < 0 || tool >= system_info_.total_slots) {
                continue;
            }
            ams::Observation stated(ams::ObservationSource::VendorCache);
            stated.spoolman_id = id > 0 ? id : 0;
            ams::ingest(lane_id(tool), stated);
        }

        if (!system_info_.units.empty()) {
            auto& slots = system_info_.units[0].slots;
            for (size_t i = 0; i < slots.size(); ++i) {
                apply_resolved_lane(slots[i], static_cast<int>(i));
            }
        }
    }
    emit_event(EVENT_STATE_CHANGED);
}

bool AmsBackendIndx::apply_seat_fault_locked() {
    const bool fault = contract_.recognized && state_.seat_suspect.value_or(-1) >= 0;
    if (fault) {
        const std::string detail = indx::motion_blocked_reason(contract_, state_);
        if (seat_fault_raised_ && system_info_.action == AmsAction::ERROR &&
            system_info_.operation_detail == detail) {
            return false;
        }
        if (!seat_fault_raised_) {
            spdlog::warn("{} {}", backend_log_tag(), detail);
        }
        seat_fault_raised_ = true;
        system_info_.action = AmsAction::ERROR;
        system_info_.operation_detail = detail;
        return true;
    }
    if (!seat_fault_raised_) {
        return false;
    }
    seat_fault_raised_ = false;
    spdlog::info("{} Half-seated fault cleared", backend_log_tag());
    if (system_info_.action == AmsAction::ERROR) {
        system_info_.action = AmsAction::IDLE;
        system_info_.operation_detail.clear();
    }
    return true;
}

bool AmsBackendIndx::merge_offsets_locked(const nlohmann::json& status) {
    if (!offsets_readable_) {
        return false;
    }
    // Absent is no news: a frame without a key keeps the value held.
    bool changed = false;
    for (int tool = 0; tool < indx::kMaxTools; ++tool) {
        for (Axis axis : kAllAxes) {
            const auto fresh =
                tool_offsets::read_tool_offset_microns(offset_reader_, status, axis, tool, "");
            if (!fresh) {
                continue;
            }
            auto [it, inserted] = tool_offsets_um_.try_emplace(std::make_pair(tool, axis), *fresh);
            if (inserted || it->second != *fresh) {
                it->second = *fresh;
                changed = true;
            }
        }
    }
    const auto global = tool_offsets::read_global_z_offset_microns(offset_reader_, status);
    if (global && global != global_z_offset_um_) {
        global_z_offset_um_ = global;
        changed = true;
    }
    return changed;
}

std::string AmsBackendIndx::nozzle_heat_blocked_reason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.active_tool || *state_.active_tool >= sized_tool_count_) {
        // No usable reading: the macros have not said what is on the head, or
        // named a tool this printer does not have.
        return {};
    }
    return indx::heat_blocked_reason(*state_.active_tool,
                                     state_.no_heat_tools.value_or(std::set<int>{}));
}

bool AmsBackendIndx::print_paused() const {
    return api_ && api_->printer_state().print_state().get_print_lifecycle() == PrintState::Paused;
}

namespace {

/// Why a swap is refused while a print is paused: the print resumes with
/// whatever nozzle is mounted.
std::string paused_swap_reason() {
    return lv_tr("Resuming would print with a different nozzle. Change tools after the print.");
}

} // namespace

std::string AmsBackendIndx::motion_blocked_reason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return indx::motion_blocked_reason(contract_, state_);
}

std::optional<helix::ErrorEvent> AmsBackendIndx::current_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!contract_.recognized) {
        return helix::make_ams_fault_event(
            helix::ErrorSource::TOOLCHANGER, lv_tr("INDX macros not recognized"),
            fmt::format(fmt::runtime(lv_tr("This printer's INDX macros are missing: {}")),
                        indx::missing_text(contract_)),
            {});
    }
    if (state_.seat_suspect.value_or(-1) >= 0) {
        return helix::make_ams_fault_event(
            helix::ErrorSource::TOOLCHANGER, lv_tr("Tool may be half seated"),
            indx::motion_blocked_reason(contract_, state_), build_recovery_actions());
    }
    return std::nullopt;
}

std::vector<helix::RecoveryAction> AmsBackendIndx::build_recovery_actions() const {
    const int tool = state_.seat_suspect.value_or(-1);
    if (!contract_.has_manual_tool_recovery || tool < 0) {
        return {};
    }
    // Both are hands-on: the user holds the tool while the latch moves.
    helix::RecoveryAction seat;
    seat.label = lv_tr("Tool is seated");
    seat.gcode = indx::manual_seat_gcode(tool);
    seat.log_tag = "indx::manual_tool_seat";
    seat.style = "primary";
    seat.confirm_message = indx::seat_confirm_message(tool);

    helix::RecoveryAction remove;
    remove.label = lv_tr("Remove tool");
    remove.gcode = indx::manual_remove_gcode();
    remove.log_tag = "indx::manual_tool_remove";
    remove.style = "danger";
    remove.confirm_message = indx::remove_confirm_message(tool);
    return {seat, remove};
}

HeadFilamentOps AmsBackendIndx::head_filament_ops(int head) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return head_filament_ops_locked(head);
}

HeadFilamentOps AmsBackendIndx::head_filament_ops_locked(int head) const {
    HeadFilamentOps ops;
    ops.unload = FilamentOpSupport::Manual;
    ops.unload_reason = lv_tr("Pick up the tool, heat it, and pull the filament out");
    const bool no_heat = state_.no_heat_tools && state_.no_heat_tools->count(head) > 0;
    if (no_heat) {
        // Shown beside the tool's own name (a picker row, a slot's menu), so
        // it does not repeat it.
        const std::string reason = lv_tr("No hotend");
        ops.load = FilamentOpSupport::Unavailable;
        ops.load_reason = reason;
        ops.unload = FilamentOpSupport::Unavailable;
        ops.unload_reason = reason;
    } else if (!contract_.has_load_filament) {
        ops.load = FilamentOpSupport::Manual;
        ops.load_reason = lv_tr("This printer has no INDX LOAD_FILAMENT macro");
    }
    return ops;
}

int AmsBackendIndx::max_heads_per_filament_op() const {
    if (!contract_.has_load_filament) {
        return 1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return std::max(sized_tool_count_, 1);
}

std::vector<AmsBackend::FeedSlotSignal> AmsBackendIndx::feed_slot_signals() const {
    std::lock_guard<std::mutex> lock(mutex_);
    FeedSlotSignal signal{"save_variables", {"variables", "active_tool"}, {}};
    for (int tool = 0; tool < sized_tool_count_; ++tool) {
        signal.value_slots.emplace_back(std::to_string(tool), tool);
    }
    if (signal.value_slots.empty()) {
        return {};
    }
    return {signal};
}

AmsError AmsBackendIndx::do_change_tool(int tool_number) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        AmsError slot_valid = validate_slot_index_locked(tool_number);
        if (!slot_valid) {
            return slot_valid;
        }
        const std::string blocked = indx::motion_blocked_reason(contract_, state_);
        if (!blocked.empty()) {
            return AmsError(AmsResult::WRONG_STATE, "INDX tool change refused", blocked, "");
        }
        if (system_info_.current_slot == tool_number) {
            return AmsErrorHelper::success();
        }
    }
    if (print_paused()) {
        return AmsError(AmsResult::WRONG_STATE, "INDX tool change refused while paused",
                        paused_swap_reason(), "");
    }
    if (tool_number < 0 || tool_number >= static_cast<int>(change_gcodes_.size())) {
        return AmsErrorHelper::not_supported("INDX tool " + std::to_string(tool_number));
    }
    std::string cmd = change_gcodes_[static_cast<size_t>(tool_number)];
    spdlog::info("{} Changing to tool {}: {}", backend_log_tag(), tool_number, cmd);
    return dispatch_operation(std::move(cmd), AmsAction::SELECTING);
}

AmsError AmsBackendIndx::do_park_active_tool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string blocked = indx::motion_blocked_reason(contract_, state_);
        if (!blocked.empty()) {
            return AmsError(AmsResult::WRONG_STATE, "INDX park refused", blocked, "");
        }
        if (system_info_.current_tool < 0) {
            return AmsErrorHelper::not_loaded();
        }
    }
    if (print_paused()) {
        return AmsError(AmsResult::WRONG_STATE, "INDX park refused while paused",
                        paused_swap_reason(), "");
    }
    spdlog::info("{} Parking the current tool: {}", backend_log_tag(), indx::park_gcode());
    return dispatch_operation(indx::park_gcode(), AmsAction::UNLOADING);
}

AmsError AmsBackendIndx::load_refusal_locked(int slot) const {
    AmsError slot_valid = validate_slot_index_locked(slot);
    if (!slot_valid) {
        return slot_valid;
    }
    const std::string blocked = indx::motion_blocked_reason(contract_, state_);
    if (!blocked.empty()) {
        return AmsError(AmsResult::WRONG_STATE, "INDX load refused", blocked, "");
    }
    if (state_.no_heat_tools && state_.no_heat_tools->count(slot) > 0) {
        // A refusal stands alone, so it names the tool.
        return AmsError(AmsResult::NOT_SUPPORTED, "INDX load unavailable",
                        indx::heat_blocked_reason(slot, {slot}), "");
    }
    const HeadFilamentOps ops = head_filament_ops_locked(slot);
    if (ops.load != FilamentOpSupport::Automatic) {
        return AmsError(AmsResult::NOT_SUPPORTED, "INDX load unavailable", ops.load_reason, "");
    }
    // LOAD_FILAMENT picks its tool up first: only the mounted one can be
    // loaded without swapping nozzles under a paused print.
    if (slot != system_info_.current_slot && print_paused()) {
        return AmsError(AmsResult::WRONG_STATE, "INDX load of a docked tool while paused",
                        paused_swap_reason(), "");
    }
    return AmsErrorHelper::success();
}

namespace {

/// The LOAD_FILAMENT line for @p slot, by its material's preset or a temperature.
std::string load_line(int slot_index, const SlotInfo& slot,
                      const std::map<std::string, indx::FilamentPreset>& presets) {
    const int temp_c =
        helix::ui::resolve_load_preheat_material(slot_index, &slot, nullptr)
            .value_or(helix::ui::PreheatTarget{app_constants::ams::kDefaultLoadPreheatTemp, ""})
            .temp_c;
    return indx::load_gcode(slot_index, slot.material, temp_c, presets);
}

} // namespace

AmsError AmsBackendIndx::do_load_filament(int slot_index) {
    SlotInfo slot;
    std::map<std::string, indx::FilamentPreset> presets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (AmsError refused = load_refusal_locked(slot_index); !refused) {
            return refused;
        }
        if (const SlotInfo* lane = system_info_.get_slot_global(slot_index)) {
            slot = *lane;
        }
        presets = state_.presets.value_or(presets);
    }
    std::string cmd = load_line(slot_index, slot, presets);
    spdlog::info("{} Loading tool {}: {}", backend_log_tag(), slot_index, cmd);
    return dispatch_operation(std::move(cmd), AmsAction::LOADING);
}

AmsError AmsBackendIndx::do_filament_batch(const std::vector<int>& slots, bool load) {
    if (!load) {
        return AmsError(AmsResult::NOT_SUPPORTED, "INDX batch unload",
                        lv_tr("Pick up the tool, heat it, and pull the filament out"), "");
    }
    if (slots.empty()) {
        return AmsErrorHelper::invalid_parameter("INDX batch load of no tools");
    }
    std::vector<SlotInfo> infos;
    std::map<std::string, indx::FilamentPreset> presets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int slot : slots) {
            if (AmsError refused = load_refusal_locked(slot); !refused) {
                return refused;
            }
            const SlotInfo* lane = system_info_.get_slot_global(slot);
            infos.push_back(lane ? *lane : SlotInfo{});
        }
        presets = state_.presets.value_or(presets);
    }
    std::string script;
    for (size_t i = 0; i < slots.size(); ++i) {
        script += (script.empty() ? "" : "\n") + load_line(slots[i], infos[i], presets);
    }
    spdlog::info("{} Loading {} tools: {}", backend_log_tag(), slots.size(), script);
    return dispatch_operation(std::move(script), AmsAction::LOADING,
                              static_cast<uint32_t>(slots.size()) *
                                  IMoonrakerAPI::kAmsOperationTimeoutMs);
}

// ============================================================================
// Device actions
// ============================================================================

namespace {

constexpr const char* kSpeedModeAction = "speed_mode";
constexpr const char* kSeatByHandAction = "seat_by_hand";
constexpr const char* kCalibrateHeaterAction = "calibrate_heater";
constexpr const char* kCalibrateFanAction = "calibrate_fan";
constexpr const char* kCycleAllAction = "cycle_all";

/// "X 0.120  Y -0.045  Z 0.030 mm", each unknown axis "-"; "-" when none is known.
std::string offsets_text(const std::map<std::pair<int, Axis>, int>& offsets, int tool) {
    std::string text;
    bool any = false;
    for (Axis axis : kAllAxes) {
        auto it = offsets.find(std::make_pair(tool, axis));
        std::string value = "-";
        if (it != offsets.end()) {
            value = fmt::format("{:.3f}", it->second / 1000.0);
            any = true;
        }
        text += fmt::format("{}{} {}", text.empty() ? "" : "  ",
                            static_cast<char>(axis_letter(axis) - 'a' + 'A'), value);
    }
    return any ? text + " mm" : "-";
}

std::optional<indx::HeaterCalibration> calibration_for(const std::string& action_id) {
    if (action_id == kCalibrateHeaterAction) {
        return indx::HeaterCalibration::Heater;
    }
    if (action_id == kCalibrateFanAction) {
        return indx::HeaterCalibration::Fan;
    }
    return std::nullopt;
}

} // namespace

double AmsBackendIndx::nozzle_target_c() const {
    if (!api_) {
        return 0.0;
    }
    return lv_subject_get_int(
               api_->printer_state().temperature_state().get_active_extruder_target_subject()) /
           10.0;
}

std::vector<helix::printer::DeviceSection> AmsBackendIndx::get_device_sections() const {
    auto sections = AmsBackendToolChanger::get_device_sections();
    const auto actions = get_device_actions();
    const auto has = [&actions](const char* section) {
        return std::any_of(actions.begin(), actions.end(),
                           [section](const auto& a) { return a.section == section; });
    };
    using helix::printer::device_section;
    if (has("speed")) {
        sections.push_back(
            device_section("speed", 0, TR_NOOP("How fast the head moves while changing tools")));
    }
    if (has("calibration")) {
        sections.push_back(
            device_section("calibration", 1, TR_NOOP("Fit the induction heater's safety model")));
    }
    if (has("maintenance")) {
        sections.push_back(device_section("maintenance", 2, TR_NOOP("Check every dock")));
    }
    if (has("tool_offsets")) {
        sections.push_back(
            device_section("tool_offsets", 3, TR_NOOP("Measured offsets, read-only")));
    }
    return sections;
}

std::vector<helix::printer::DeviceAction> AmsBackendIndx::get_device_actions() const {
    using DA = helix::printer::DeviceAction;
    auto actions = AmsBackendToolChanger::get_device_actions();
    if (!contract_.recognized) {
        return actions;
    }
    const double target_c = nozzle_target_c();

    std::lock_guard<std::mutex> lock(mutex_);
    if (contract_.has_speed_modes) {
        std::any current;
        if (state_.speed_mode) {
            if (auto mode = indx::parse_speed_mode(*state_.speed_mode)) {
                current = std::string(indx::speed_mode_option(*mode));
            }
        }
        std::vector<std::string> options;
        for (auto mode : indx::kSpeedModes) {
            options.emplace_back(indx::speed_mode_option(mode));
        }
        actions.push_back(DA::dropdown("speed_mode", "Tool change speed", "speed", options, current,
                                       "speed", "Stealth is quiet, Sport is fastest"));
    }

    // One device action on the printer at a time: the rest grey until it ends.
    const std::string running_reason =
        device_action_running() ? lv_tr("Wait for the current operation to complete") : "";
    const auto gate = [&running_reason](DA& action, std::string reason) {
        action.disable_reason = reason.empty() ? running_reason : std::move(reason);
        action.enabled = action.disable_reason.empty();
    };
    const auto add_calibration = [&](indx::HeaterCalibration which, DA action) {
        action.guided_flow = which == indx::HeaterCalibration::Heater ? "indx_heater_calibration"
                                                                      : "indx_fan_calibration";
        gate(action, indx::calibration_blocked_reason(contract_, state_, which, target_c));
        actions.push_back(std::move(action));
    };
    add_calibration(indx::HeaterCalibration::Heater,
                    DA::button("calibrate_heater", "Calibrate heater", "calibration", "fire",
                               "Fit the heater model with the part fan off")
                        .with_machine_motion());
    add_calibration(indx::HeaterCalibration::Fan,
                    DA::button("calibrate_fan", "Calibrate part fan", "calibration", "fan",
                               "Fit how the part fan cools the nozzle")
                        .with_machine_motion());

    if (contract_.has_manual_tool_seat) {
        DA seat = DA::button("seat_by_hand", "Seat a tool by hand", "calibration", "lock",
                             "For a first Z home: claim the tool you put on the head")
                      .with_machine_motion();
        seat.guided_flow = indx::kSeatByHandFlowId;
        gate(seat, {});
        actions.push_back(std::move(seat));
    }

    if (contract_.has_cycle_all) {
        DA cycle = DA::button("cycle_all", "Cycle every tool", "maintenance", "swap_horizontal",
                              "Pick up and park each tool in turn")
                       .with_machine_motion();
        cycle.guided_flow = "indx_cycle_all";
        gate(cycle, indx::motion_blocked_reason(contract_, state_));
        actions.push_back(std::move(cycle));
    }

    if (offsets_readable_) {
        for (int tool = 0; tool < std::max(sized_tool_count_, 0); ++tool) {
            DA row = DA::info("tool_offset", "Offset", "tool_offsets",
                              offsets_text(tool_offsets_um_, tool));
            row.tool_index = tool;
            actions.push_back(std::move(row));
        }
        actions.push_back(DA::info("global_z_offset", "Global Z offset", "tool_offsets",
                                   global_z_offset_um_
                                       ? fmt::format("{:.3f} mm", *global_z_offset_um_ / 1000.0)
                                       : std::string("-")));
    }
    return actions;
}

AmsError AmsBackendIndx::do_execute_device_action(const std::string& action_id,
                                                  const std::any& value) {
    if (action_id == kSeatByHandAction) {
        const auto [tool, err] = printer::device_action_value<int>(value, "tool");
        if (!err) {
            return err;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (tool < 0 || tool >= sized_tool_count_) {
                return AmsErrorHelper::invalid_parameter("INDX tool " + std::to_string(tool));
            }
        }
        spdlog::info("{} Seating tool {} by hand", backend_log_tag(), tool);
        return execute_gcode(indx::manual_seat_gcode(tool));
    }
    if (action_id == kSpeedModeAction) {
        const auto [text, err] = printer::device_action_value<std::string>(value, "speed mode");
        if (!err) {
            return err;
        }
        const auto mode = indx::parse_speed_mode(text);
        if (!mode) {
            return AmsErrorHelper::invalid_parameter("INDX speed mode '" + text + "'");
        }
        spdlog::info("{} Speed mode {}", backend_log_tag(), indx::speed_mode_option(*mode));
        return execute_gcode(indx::speed_mode_gcode(*mode));
    }
    if (action_id == kCycleAllAction) {
        return execute_gcode(indx::cycle_all_gcode());
    }
    if (auto which = calibration_for(action_id)) {
        return execute_gcode(indx::calibration_gcode(*which));
    }
    return AmsBackendToolChanger::do_execute_device_action(action_id, value);
}

AmsError AmsBackendIndx::do_run_device_action(const std::string& action_id, const std::any& value,
                                              DeviceActionDone on_done) {
    std::string cmd;
    if (action_id == kCycleAllAction) {
        cmd = indx::cycle_all_gcode();
    } else if (auto which = calibration_for(action_id)) {
        cmd = indx::calibration_gcode(*which);
    } else {
        return AmsBackendToolChanger::do_run_device_action(action_id, value, std::move(on_done));
    }
    if (!begin_tracked_run()) {
        return AmsErrorHelper::busy("a device action");
    }
    spdlog::info("{} Running {} (tracked)", backend_log_tag(), cmd);
    emit_event(EVENT_STATE_CHANGED);
    // The macros home themselves. Both answers arrive on the main thread,
    // behind the backend's lifetime token, so `this` is alive when they run.
    auto done = std::make_shared<DeviceActionDone>(std::move(on_done));
    AmsError sent = ensure_homed_then(
        cmd,
        [this, done]() {
            end_tracked_run();
            emit_event(EVENT_STATE_CHANGED);
            if (*done) {
                (*done)(AmsErrorHelper::success());
            }
        },
        [this, done, cmd](const MoonrakerError& err) {
            end_tracked_run();
            emit_event(EVENT_STATE_CHANGED);
            if (!*done) {
                return;
            }
            // No answer is not a failure: the macro may still be running.
            if (err.type == MoonrakerErrorType::TIMEOUT ||
                err.type == MoonrakerErrorType::CONNECTION_LOST) {
                (*done)(AmsErrorHelper::timeout(cmd));
                return;
            }
            // Klipper's own words, so a screen tracking the run can say why.
            AmsError failed = AmsErrorHelper::command_failed(cmd, err.message);
            failed.user_msg = err.localized_message();
            (*done)(failed);
        },
        indx::kCalibrationTimeoutMs, /*skip_homing=*/true, /*silent=*/true,
        /*caller_surfaces_errors=*/false);
    if (!sent) {
        end_tracked_run();
    }
    return sent;
}

// ============================================================================
// Edit follow-up: the heater model after a material edit
// ============================================================================

AmsError AmsBackendIndx::follow_up_refusal_locked(int slot_index, const SlotInfo& before,
                                                  const SlotInfo& after, std::string& preset_key,
                                                  indx::FilamentPreset& values) const {
    const auto refuse = [](const char* why) {
        return AmsError(AmsResult::WRONG_STATE, std::string("INDX heater model update: ") + why,
                        lv_tr("Not available now"), "");
    };
    if (!contract_.recognized || !contract_.has_set_tool_filament) {
        return AmsErrorHelper::not_supported("SET_TOOL_FILAMENT");
    }
    if (AmsError slot_valid = validate_slot_index_locked(slot_index); !slot_valid) {
        return slot_valid;
    }
    const auto presets = state_.presets.value_or(std::map<std::string, indx::FilamentPreset>{});
    const auto preset = indx::preset_for_material(after.material, presets);
    if (!preset) {
        return AmsErrorHelper::invalid_parameter("material '" + after.material + "'");
    }
    if (indx::preset_for_material(before.material, presets) == preset) {
        return refuse("the preset did not change");
    }
    if (state_.no_heat_tools && state_.no_heat_tools->count(slot_index) > 0) {
        return refuse("the tool has no hotend");
    }
    // SET_TOOL_FILAMENT also loads the values into the live heater model,
    // which belongs to whatever tool is on the head.
    const int mounted = state_.active_tool.value_or(-2);
    if (mounted != slot_index && mounted != -1) {
        return refuse("another tool is mounted");
    }
    const auto it = presets.find(*preset);
    if (it == presets.end()) {
        return AmsErrorHelper::invalid_parameter("material '" + after.material + "'");
    }
    preset_key = *preset;
    values = it->second;
    return AmsErrorHelper::success();
}

std::optional<AmsBackend::EditFollowUp>
AmsBackendIndx::edit_follow_up(int slot_index, const SlotInfo& before,
                               const SlotInfo& after) const {
    if (print_holds_printer()) {
        return std::nullopt;
    }
    std::string preset;
    indx::FilamentPreset values;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!follow_up_refusal_locked(slot_index, before, after, preset, values)) {
            return std::nullopt;
        }
    }
    EditFollowUp offer;
    offer.title = lv_tr("Update heater model?");
    offer.message = fmt::format(
        fmt::runtime(lv_tr("Update the INDX heater model too? {} now holds {}: density {:.2f}, "
                           "heat capacity {:.2f}.")),
        indx::tool_text(slot_index), preset, values.density, values.heat_capacity);
    offer.confirm_label = lv_tr("Update");
    return offer;
}

AmsError AmsBackendIndx::do_run_edit_follow_up(int slot_index, const SlotInfo& before,
                                               const SlotInfo& after) {
    std::string preset;
    indx::FilamentPreset values;
    {
        // Asked again: the head or the presets may have changed while the
        // dialog was up.
        std::lock_guard<std::mutex> lock(mutex_);
        if (AmsError refused = follow_up_refusal_locked(slot_index, before, after, preset, values);
            !refused) {
            return refused;
        }
    }
    std::string cmd = indx::set_tool_filament_gcode(slot_index, values);
    spdlog::info("{} Updating the heater model: {}", backend_log_tag(), cmd);
    return execute_gcode(cmd);
}

} // namespace helix
