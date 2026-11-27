// tests/test_helpers/happy_hare_test_fixtures.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What these test files share: their fixtures, helpers and includes.
// test_ams_backend_happy_hare.cpp, test_ams_backend_happy_hare_2.cpp,
// test_ams_backend_happy_hare_3.cpp, test_ams_backend_happy_hare_4.cpp,
// test_ams_backend_happy_hare_5.cpp. Generated from the first of those files by split_v3.py, which
// keeps every case's text and order and moves everything else here.
#pragma once

#include "ui_ams_device_section_detail_overlay.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "action_prompt_manager.h"
#include "ams_backend_happy_hare.h"
#include "ams_state.h"
#include "ams_types.h"
#include "buffer_reading.h"
#include "config.h"
#include "core/update_queue.h"
#include "filament_mapper.h"
#include "hh_defaults.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "lvgl_ui_test_fixture.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "preflight_validator.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"
#include "spoolman_types.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/config_test_access.h"
#include "test_helpers/device_action_controls.h"
#include "test_helpers/filament_accounting_cases.h"
#include "test_helpers/gcode_recording_api.h"
#include "test_helpers/happy_hare_fixture.h"
#include "test_helpers/happy_hare_test_access.h"
#include "test_helpers/mock_printer.h"
#include "test_helpers/print_state_test_drivers.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/seeded_override.h"

#include <algorithm>
#include <any>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace helix::test {

using helix::ams_action_from_string;
using helix::AmsAction;
using helix::AmsError;
using helix::AmsResult;
using helix::AmsType;
using helix::BufferMotion;
using helix::EncoderClogInfo;
using helix::path_segment_from_happy_hare_pos;
using helix::PathSegment;
using helix::PathTopology;
using helix::SlotInfo;
using helix::SlotStatus;
using helix::spoolman_mode_from_string;
using helix::spoolman_mode_to_string;
using helix::SpoolmanMode;
using helix::SpoolMotorAction;
using helix::StepOperationType;

} // namespace helix::test

namespace helix {

/**
 * @brief Test helper class providing access to AmsBackendHappyHare internals
 *
 * This class provides controlled access to private members for unit testing.
 * It does NOT start the backend (no Moonraker connection needed).
 */
class AmsBackendHappyHareTestHelper : public AmsBackendHappyHare {
  public:
    AmsBackendHappyHareTestHelper(IMoonrakerAPI* api = nullptr,
                                  helix::IMoonrakerClient* client = nullptr)
        : AmsBackendHappyHare(api, client), has_api_(api != nullptr) {}

    /**
     * @brief Initialize test gates with default SlotInfo
     * @param count Number of gates to create
     */
    /// Feed a printer.mmu gate_spool_id array, as a v4 status update would.
    void feed_mmu_gate_spool_ids(const std::vector<int>& ids) {
        nlohmann::json mmu;
        mmu["gate_spool_id"] = ids;
        nlohmann::json params;
        params["mmu"] = mmu;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    /// Feed an arbitrary printer.mmu status object.
    void feed_mmu_status(const nlohmann::json& mmu) {
        nlohmann::json params;
        params["mmu"] = mmu;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    /// Feed a printer.mmu gate_status array (0 empty, 1 available, 2 loaded),
    /// as a status update would.
    void feed_mmu_gate_status(const std::vector<int>& statuses) {
        nlohmann::json mmu;
        mmu["gate_status"] = statuses;
        nlohmann::json params;
        params["mmu"] = mmu;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    /// Seed a user override directly (no persist round-trip), as the AFC
    /// override tests do. Callers hold no lock; this takes mutex_.
    void set_gate_override(int slot_index, const helix::ams::FilamentSlotOverride& o) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            HappyHareTestAccess::overrides(*this)[slot_index] = o;
        }
        // The backend's own init files both stores together.
        helix::test::file_override_as_lane_records(*this, slot_index, o);
    }

    [[nodiscard]] bool has_gate_override(int slot_index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return HappyHareTestAccess::overrides(*this).count(slot_index) > 0;
    }

    /// Put a whole Spoolman link on the live slot — the shape a linked slot
    /// carries. The persisted override record has no filament-id field, so
    /// the live slot is the only half of a link a test can seed whole.
    void set_gate_spoolman_link(int slot_index, int spool_id, int filament_id, int vendor_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* entry = HappyHareTestAccess::slots(*this).get_mut(slot_index);
        if (entry) {
            entry->info.spoolman_id = spool_id;
            entry->info.spoolman_filament_id = filament_id;
            entry->info.spoolman_vendor_id = vendor_id;
            entry->info.spool_name = "Linked Spool";
        }
    }

    void clear_slot_override(int slot_index) {
        AmsBackendHappyHare::clear_slot_override(slot_index);
    }

    void initialize_test_gates(int count) {
        system_info_.units.clear();
        // Real backend reads this from mmu.endless_spool_enabled; GROUPS= writes
        // are refused while it is off.
        system_info_.endless_spool_enabled = true;

        AmsUnit unit;
        unit.unit_index = 0;
        unit.name = "Happy Hare MMU";
        unit.slot_count = count;
        unit.first_slot_global_index = 0;

        for (int i = 0; i < count; ++i) {
            SlotInfo slot;
            slot.slot_index = i;
            slot.global_index = i;
            slot.status = SlotStatus::AVAILABLE;
            slot.mapped_tool = i;
            slot.color_rgb = helix::kAmsDefaultSlotColor;
            unit.slots.push_back(slot);
        }

        unit.topology =
            HappyHareTestAccess::is_type_b(*this) ? PathTopology::HUB : PathTopology::LINEAR;
        unit.has_encoder = !HappyHareTestAccess::is_type_b(*this);

        system_info_.units.push_back(unit);
        system_info_.total_slots = count;

        // Also initialize tool_to_slot_map for reset_tool_mappings tests
        system_info_.tool_to_slot_map.clear();
        for (int i = 0; i < count; ++i) {
            system_info_.tool_to_slot_map.push_back(i);
        }

        // Initialize SlotRegistry to match
        std::vector<std::string> slot_names;
        for (int i = 0; i < count; ++i) {
            slot_names.push_back(std::to_string(i));
        }
        HappyHareTestAccess::slots(*this).initialize("MMU", slot_names);
        // Set status to AVAILABLE to match legacy init
        for (int i = 0; i < count; ++i) {
            auto* entry = HappyHareTestAccess::slots(*this).get_mut(i);
            if (entry) {
                entry->info.status = SlotStatus::AVAILABLE;
                entry->info.color_rgb = helix::kAmsDefaultSlotColor;
            }
        }
        // Set 1:1 tool map
        HappyHareTestAccess::slots(*this).set_tool_map(system_info_.tool_to_slot_map);
    }

    /**
     * @brief Initialize the slot registry with multiple units (per-gate testing).
     * @param gates_per_unit Gate count for each unit; total gates = sum.
     */
    void initialize_test_units(const std::vector<int>& gates_per_unit) {
        std::vector<std::pair<std::string, std::vector<std::string>>> units;
        int g = 0;
        for (size_t u = 0; u < gates_per_unit.size(); ++u) {
            std::vector<std::string> names;
            for (int i = 0; i < gates_per_unit[u]; ++i) {
                names.push_back(std::to_string(g++));
            }
            units.push_back({"MMU" + std::to_string(u), std::move(names)});
        }
        HappyHareTestAccess::slots(*this).initialize_units(units);
    }

    /**
     * @brief Populate system_info_.units AND the slot registry for multiple units.
     * Unlike initialize_test_units (registry only), this fills system_info_.units so
     * per-unit logic (e.g. heater_suffix_locked) sees the multi-unit topology.
     * @param gates_per_unit Gate count for each unit; total gates = sum.
     */
    void initialize_test_gates_multi(const std::vector<int>& gates_per_unit) {
        system_info_.units.clear();
        system_info_.endless_spool_enabled = true;
        int total = 0;
        int global = 0;
        for (size_t u = 0; u < gates_per_unit.size(); ++u) {
            AmsUnit unit;
            unit.unit_index = static_cast<int>(u);
            unit.name = "MMU" + std::to_string(u);
            unit.slot_count = gates_per_unit[u];
            unit.first_slot_global_index = global;
            for (int i = 0; i < gates_per_unit[u]; ++i) {
                SlotInfo slot;
                slot.slot_index = i;
                slot.global_index = global + i;
                slot.status = SlotStatus::AVAILABLE;
                unit.slots.push_back(slot);
            }
            global += gates_per_unit[u];
            total += gates_per_unit[u];
            system_info_.units.push_back(unit);
        }
        system_info_.total_slots = total;
        initialize_test_units(gates_per_unit);
    }

    /**
     * @brief Get mutable slot pointer for test setup
     * @param slot_index Global slot index
     * @return Pointer to SlotInfo or nullptr
     */
    SlotInfo* get_mutable_slot(int slot_index) {
        auto* entry = HappyHareTestAccess::slots(*this).get_mut(slot_index);
        return entry ? &entry->info : nullptr;
    }

    /**
     * @brief Get const SlotEntry pointer for test assertions (includes sensors)
     * @param slot_index Global slot index
     * @return Pointer to SlotEntry or nullptr
     */
    const helix::printer::SlotEntry* get_slot_entry(int slot_index) const {
        return HappyHareTestAccess::slots(*this).get(slot_index);
    }

    /**
     * @brief Pre-gate sensor state for a gate, or nullptr when no frame ever
     * reported it
     */
    const helix::HappyHareGateSensor* get_gate_sensor(int slot_index) const {
        const auto& map = HappyHareTestAccess::gate_sensors(*this);
        auto it = map.find(slot_index);
        return it != map.end() ? &it->second : nullptr;
    }

    // G-code capture for persistence tests
    std::vector<std::string> captured_gcodes;

    /**
     * @brief Override execute_gcode to capture commands for testing
     * @param gcode The G-code command
     * @return Success
     */
    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        if (!fail_gcode.empty() && gcode == fail_gcode) {
            return AmsError(AmsResult::COMMAND_FAILED, "refused", "refused", "");
        }
        return AmsErrorHelper::success();
    }

    /// The completion form (the Endless Spool switch) captures into the same list;
    /// fail_gcode answers its on_error.
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                           helix::SharedCallback<void(const MoonrakerError&)> on_error,
                           bool silent) override {
        // A test that hands in a recording API reads the sends there.
        if (has_api_) {
            return AmsBackendHappyHare::execute_gcode(gcode, std::move(on_complete),
                                                      std::move(on_error), silent);
        }
        captured_gcodes.push_back(gcode);
        if (!fail_gcode.empty() && gcode == fail_gcode) {
            if (on_error) {
                on_error(MoonrakerError{});
            }
        } else if (on_complete) {
            on_complete();
        }
        return AmsErrorHelper::success();
    }

    /// A command execute_gcode() answers with an error.
    std::string fail_gcode;

  private:
    bool has_api_ = false;

  public:
    /// Mark unit @p u so a test can tell whether the unit list was rebuilt.
    void mark_unit(int u, const std::string& marker) {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.units.at(u).firmware_version = marker;
    }
    std::string unit_marker(int u) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return system_info_.units.at(u).firmware_version;
    }

    /**
     * @brief Feed MMU JSON state through the normal notification pipeline
     * @param mmu_data JSON object representing printer.mmu data
     */
    void test_parse_mmu_state(const nlohmann::json& mmu_data) {
        nlohmann::json notification;
        nlohmann::json params;
        params["mmu"] = mmu_data;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    void set_running(bool state) {
        running_ = state;
    }

    void set_filament_loaded(bool state) {
        system_info_.filament_loaded = state;
    }

    void set_current_slot(int slot) {
        system_info_.current_slot = slot;
    }

    void set_supports_bypass(bool state) {
        system_info_.supports_bypass = state;
    }

    void set_spoolman_mode(SpoolmanMode mode) {
        system_info_.spoolman_mode = mode;
    }

    /// Feed a printer.mmu status object in a frame stamped with Klipper's @p eventtime.
    void feed_mmu_status_at(const nlohmann::json& mmu, double eventtime) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({{{"mmu", mmu}}, eventtime});
        handle_status_update(notification);
    }

    void set_selector_type(const std::string& type) {
        HappyHareTestAccess::selector_type(*this) = type;
    }

    /**
     * @brief Set config defaults for device actions testing
     *
     * Populates config_defaults_ with known values so tests can verify
     * that get_device_actions() overlays them correctly.
     */
    void set_config_defaults_for_test() {
        HappyHareTestAccess::config_defaults(*this).gear_from_buffer_speed = 180.0f;
        HappyHareTestAccess::config_defaults(*this).gear_from_spool_speed = 70.0f;
        HappyHareTestAccess::config_defaults(*this).gear_unload_speed = 90.0f;
        HappyHareTestAccess::config_defaults(*this).selector_move_speed = 200.0f;
        HappyHareTestAccess::config_defaults(*this).extruder_load_speed = 45.0f;
        HappyHareTestAccess::config_defaults(*this).extruder_unload_speed = 45.0f;
        HappyHareTestAccess::config_defaults(*this).toolhead_sensor_to_nozzle = 62.0f;
        HappyHareTestAccess::config_defaults(*this).toolhead_extruder_to_nozzle = 72.0f;
        HappyHareTestAccess::config_defaults(*this).toolhead_entry_to_extruder = 0.0f;
        HappyHareTestAccess::config_defaults(*this).toolhead_ooze_reduction = 2.0f;
        HappyHareTestAccess::config_defaults(*this).sync_to_extruder = 0;
        HappyHareTestAccess::config_defaults(*this).clog_detection = 0;
        HappyHareTestAccess::config_defaults(*this).loaded = true;
    }

    void apply_selector_type_update() {
        HappyHareTestAccess::update_unit_topologies(*this);
    }

    void clear_captured_gcodes() {
        captured_gcodes.clear();
    }

    /// Expose reapply_overrides for testing
    void test_reapply_overrides() {
        HappyHareTestAccess::reapply_overrides(*this);
    }

    /// Override the clock function for deterministic countdown tests
    void test_set_clock(std::function<std::time_t()> fn) {
        HappyHareTestAccess::now_fn(*this) = std::move(fn);
    }

    /// Expose apply_heater_config for testing (simulates the async configfile query result)
    void test_apply_heater_config(const nlohmann::json& settings) {
        test_apply_heater_config(settings, nlohmann::json::object());
    }

    /// Same, with the live mmu_machine object Happy Hare v4 carries the fields in.
    void test_apply_heater_config(const nlohmann::json& settings,
                                  const nlohmann::json& live_mmu_machine) {
        HappyHareTestAccess::apply_heater_config(
            *this, settings, live_mmu_machine,
            happy_hare::read_machine_layout(settings, live_mmu_machine));
    }

    /// Expose apply_filament_heater_status for testing
    bool test_apply_filament_heater_status(const nlohmann::json& params) {
        return HappyHareTestAccess::apply_filament_heater_status(*this, params);
    }

    /// Expose apply_environment_sensor_status for testing
    bool test_apply_environment_sensor_status(const nlohmann::json& params) {
        return HappyHareTestAccess::apply_environment_sensor_status(*this, params);
    }

    /**
     * @brief Check if exact G-code was captured
     * @param expected Exact G-code string to find
     * @return true if found
     */
    bool has_gcode(const std::string& expected) const {
        return std::find(captured_gcodes.begin(), captured_gcodes.end(), expected) !=
               captured_gcodes.end();
    }

    /**
     * @brief Check if any captured G-code starts with prefix
     * @param prefix Prefix to search for
     * @return true if any G-code starts with prefix
     */
    bool has_gcode_starting_with(const std::string& prefix) const {
        for (const auto& gcode : captured_gcodes) {
            if (gcode.rfind(prefix, 0) == 0)
                return true;
        }
        return false;
    }

    /**
     * @brief Check if any captured G-code contains substring
     * @param substring String to search for
     * @return true if any G-code contains substring
     */
    bool has_gcode_containing(const std::string& substring) const {
        for (const auto& gcode : captured_gcodes) {
            if (gcode.find(substring) != std::string::npos)
                return true;
        }
        return false;
    }
};
} // namespace helix

namespace helix::test {

using helix::AmsBackendHappyHareTestHelper;
inline const helix::printer::DeviceAction*
find_action(const std::vector<helix::printer::DeviceAction>& all, const std::string& id) {
    for (const auto& a : all) {
        if (a.id == id) {
            return &a;
        }
    }
    return nullptr;
}

struct HhHelper {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
};

struct HhHelperWithClient {
    helix::test::FakeMoonrakerClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg{nullptr, &client};
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
};

template <typename Base> struct HhFourGatesOf : Base {
    HhFourGatesOf() {
        this->helper.initialize_test_gates(4);
    }
};

using HhFourGates = HhFourGatesOf<HhHelper>;

using HhFourGatesWithClient = HhFourGatesOf<HhHelperWithClient>;

static SlotInfo funnel_cleared_copy(const SlotInfo& original) {
    SlotInfo cleared = original;
    cleared.material.clear();
    cleared.color_rgb = helix::kAmsDefaultSlotColor;
    cleared.color_name.clear();
    cleared.multi_color_hexes.clear();
    cleared.brand.clear();
    cleared.catalog_id.clear();
    cleared.product_name.clear();
    cleared.clear_spoolman_link();
    cleared.remaining_weight_g = -1;
    cleared.total_weight_g = -1;
    return cleared;
}

inline void connect_with_fixture(AmsBackendHappyHareTestHelper& helper,
                                 helix::test::FakeMoonrakerClient& client,
                                 const nlohmann::json& fx);

struct PrintLifecycleFixture : public LVGLTestFixture {
    PrintLifecycleFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
    }

    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
};

inline helix::RecoverStateRequest recover_req(int slot, bool bypass, std::optional<bool> loaded) {
    helix::RecoverStateRequest r;
    r.slot = slot;
    r.bypass = bypass;
    r.loaded = loaded;
    return r;
}

inline void connect_with_fixture(AmsBackendHappyHareTestHelper& helper,
                                 helix::test::FakeMoonrakerClient& client,
                                 const nlohmann::json& fx);

/// The single-unit v4 fixture, reporting Happy Hare @p version.
inline nlohmann::json happy_hare_fixture_at(const std::string& version) {
    nlohmann::json fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
    fx["mmu_machine"]["happy_hare_version"] = version;
    fx["configfile_settings"]["mmu_machine"]["happy_hare_version"] = version;
    return fx;
}

inline bool has_recovery(const helix::ErrorEvent& ev, const std::string& tag) {
    for (const auto& a : ev.recovery_actions)
        if (a.log_tag == tag)
            return true;
    return false;
}

constexpr const char* kObjectsQuery = "printer.objects.query";

inline std::string join_ints(const std::vector<int>& values) {
    std::string out;
    for (const int v : values) {
        out += (out.empty() ? "" : ",") + std::to_string(v);
    }
    return out;
}

struct HappyHareGoldenFixture : public LVGLUITestFixture {
    HappyHareGoldenFixture() {
        backend.set_event_callback(
            [this](const std::string& name, const std::string&) { events.push_back(name); });
        helix::ui::set_test_toast_hook([this](ToastSeverity, const std::string& message, uint32_t) {
            toasts += (toasts.empty() ? "" : "|") + message;
        });
    }

    ~HappyHareGoldenFixture() override {
        helix::ui::set_test_toast_hook(nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    std::string feed(const nlohmann::json& mmu) {
        events.clear();
        toasts.clear();
        nlohmann::json params;
        params["mmu"] = mmu;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        HappyHareTestAccess::handle_status_update(backend, notification);
        helix::ui::UpdateQueue::instance().drain();
        return snapshot();
    }

    std::string snapshot() {
        using A = HappyHareTestAccess;
        const helix::AmsSystemInfo info = backend.get_system_info();
        std::string out = fmt::format(
            "slot={} tool={} loaded={} action={} detail='{}' bypass={} units={} hub={} "
            "slot_sensors={}\n",
            info.current_slot, info.current_tool, info.filament_loaded,
            helix::ams_action_to_string(info.action), info.operation_detail, info.supports_bypass,
            A::num_units(backend), info.units.empty() ? false : info.units[0].hub_sensor_triggered,
            info.units.empty() ? false : info.units[0].has_slot_sensors);
        out += fmt::format("  pos={} bowden={} unit={} counts={} seg={} fault='{}' rawgates={}\n",
                           A::filament_pos(backend), A::bowden_progress(backend),
                           A::active_unit(backend), join_ints(A::per_unit_gate_counts(backend)),
                           helix::path_segment_to_string(A::error_segment(backend)),
                           A::reason_for_pause(backend), join_ints(A::gate_status_raw(backend)));
        out += fmt::format(
            "  espooler='{}' fb='{}' bias={:.3f}/{:.3f} drive={} clog={} enc={}/{}/{:.1f}/{:.1f}/"
            "{:.1f}/{:.1f} fg={}/{}/{}/{:.2f}/{:.2f}/{:.2f} fgmode={} led='{}' flow={:.1f} "
            "purge={:.1f}\n",
            info.espooler_state, info.sync_feedback_state, info.sync_feedback_bias,
            info.sync_feedback_bias_raw, info.sync_drive, info.clog_detection,
            info.encoder_info.enabled, info.encoder_info.flow_rate,
            info.encoder_info.desired_headroom, info.encoder_info.detection_length,
            info.encoder_info.headroom, info.encoder_info.min_headroom, info.flowguard_info.enabled,
            info.flowguard_info.active, info.flowguard_info.trigger, info.flowguard_info.level,
            info.flowguard_info.max_clog, info.flowguard_info.max_tangle,
            A::flowguard_encoder_mode(backend), A::led_exit_effect(backend),
            info.sync_feedback_flow_rate, info.toolchange_purge_volume);
        out += fmt::format(
            "  tc={}/{} spoolman={} pending={} endless={} ttg={}\n", info.current_toolchange,
            info.number_of_toolchanges, helix::spoolman_mode_to_string(info.spoolman_mode),
            info.pending_spool_id, info.endless_spool_enabled, join_ints(info.tool_to_slot_map));
        const auto dryer = backend.get_dryer_info();
        std::string drying;
        for (const auto& d : A::gate_drying_states(backend)) {
            drying += d + "|";
        }
        out += fmt::format("  dryer={}/{}/{:.1f}/{:.1f}/{}/{}/{} gatedry={}\n", dryer.supported,
                           dryer.active, dryer.current_temp_c, dryer.target_temp_c,
                           dryer.remaining_min, dryer.duration_min, dryer.fan_pct, drying);
        for (int i = 0; i < 4; ++i) {
            const SlotInfo s = backend.get_slot_info(i);
            const auto* sensor = HappyHareTestAccess::gate_sensor(backend, i);
            out += fmt::format(
                "  g{} {} #{:06X} mat='{}' name='{}' brand='{}' spool={} temp={}-{} grp={} err={} "
                "sensor={}/{}\n",
                i, helix::slot_status_to_string(s.status), s.color_rgb, s.material, s.color_name,
                s.brand, s.spoolman_id, s.nozzle_temp_min, s.nozzle_temp_max, s.endless_spool_group,
                s.error ? s.error->message : std::string("-"),
                sensor ? sensor->has_pre_gate_sensor : false,
                sensor ? sensor->pre_gate_triggered : false);
        }
        out += fmt::format("  events={} toasts='{}'", events.size(), toasts);
        return out;
    }

    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> backend_reg{nullptr, nullptr};
    AmsBackendHappyHareTestHelper& backend = *backend_reg;
    std::vector<std::string> events;
    std::string toasts;
};

inline nlohmann::json golden_hh_full_frame() {
    using json = nlohmann::json;
    return json{
        {"gate", 1},
        {"tool", 1},
        {"filament", "Loaded"},
        {"reason_for_pause", ""},
        {"action", "Idle"},
        {"filament_pos", 8},
        {"bowden_progress", -1},
        {"has_bypass", true},
        {"num_units", 1},
        {"num_gates", 4},
        {"unit", 0},
        {"gate_status", json::array({1, 2, 0, -1})},
        {"gate_color_rgb", json::array({0xFF0000, 0x00FF00, json::array({1.0, 0.5, 0.0}), 0})},
        {"gate_color", json::array({"0000ff", "0000ff", "0000ff", "0000ff"})},
        {"gate_material", json::array({"PLA", "PETG", "ABS", ""})},
        {"espooler_active", "assist"},
        {"sync_feedback_state", "neutral"},
        {"sync_feedback_bias_modelled", 0.25},
        {"sync_feedback_bias_raw", -0.5},
        {"sync_drive", true},
        {"clog_detection_enabled", 2},
        {"encoder", json{{"flow_rate", 97},
                         {"desired_headroom", 8.0},
                         {"detection_length", 20.0},
                         {"headroom", 15.0},
                         {"min_headroom", 6.0}}},
        {"flowguard", json{{"enabled", true},
                           {"active", false},
                           {"trigger", ""},
                           {"level", 0.1},
                           {"max_clog", 0.8},
                           {"max_tangle", -0.8},
                           {"encoder_mode", 1}}},
        {"leds", json{{"unit0", json{{"exit_effect", "gate_status"}}}}},
        {"sync_feedback_flow_rate", 99.5},
        {"toolchange_purge_volume", 45.0},
        {"num_toolchanges", 3},
        {"slicer_tool_map", json{{"total_toolchanges", 10}}},
        {"spoolman_support", "pull"},
        {"pending_spool_id", 7},
        {"gate_spool_id", json::array({12, 0, -1, 5})},
        {"gate_temperature", json::array({210, 230, 250, 200})},
        {"gate_name", json::array({"Red", "", "Orange", "Blue"})},
        {"gate_filament_name", json::array({"Ignored", "Green PETG", "", ""})},
        {"ttg_map", json::array({1, 0, 2, 3})},
        {"sensors", json{{"mmu_pre_gate_0", true},
                         {"mmu_pre_gate_1", false},
                         {"mmu_pre_gate_2", nullptr},
                         {"mmu_gear", true}}},
        {"drying_state", json{{"active", true},
                              {"current_temp", 41.5},
                              {"target_temp", 55.0},
                              {"remaining_min", 120},
                              {"duration_min", 240},
                              {"fan_pct", 30}}},
        {"endless_spool_enabled", 1},
        {"endless_spool_groups", json::array({0, 0, 1, 1})}};
}

const std::string kHappyHareGolden = R"GOLD(full frame
slot=1 tool=1 loaded=true action=Idle detail='' bypass=true units=1 hub=true slot_sensors=true
  pos=8 bowden=-1 unit=0 counts=4 seg=None fault='' rawgates=1,2,0,-1
  espooler='assist' fb='neutral' bias=0.250/-0.500 drive=true clog=2 enc=true/97/8.0/20.0/15.0/6.0 fg=true/false//0.10/0.80/-0.80 fgmode=1 led='gate_status' flow=99.5 purge=45.0
  tc=2/10 spoolman=Pull pending=7 endless=true ttg=1,0,2,3
  dryer=true/true/41.5/55.0/120/240/30 gatedry=
  g0 Available #FF0000 mat='PLA' name='Red' brand='' spool=12 temp=210-210 grp=0 err=- sensor=true/true
  g1 Loaded #00FF00 mat='PETG' name='Green PETG' brand='' spool=0 temp=230-230 grp=0 err=- sensor=true/false
  g2 Empty #FF8000 mat='ABS' name='Orange' brand='' spool=0 temp=250-250 grp=1 err=- sensor=true/false
  g3 Unknown #000000 mat='' name='Blue' brand='' spool=5 temp=200-200 grp=1 err=- sensor=false/false
  events=1 toasts=''
load: loading
slot=1 tool=1 loaded=true action=Loading detail='' bypass=true units=1 hub=true slot_sensors=true
  pos=3 bowden=40 unit=0 counts=4 seg=None fault='' rawgates=1,2,0,-1
  espooler='assist' fb='neutral' bias=0.250/-0.500 drive=true clog=2 enc=true/97/8.0/20.0/15.0/6.0 fg=true/false//0.10/0.80/-0.80 fgmode=1 led='gate_status' flow=99.5 purge=45.0
  tc=2/10 spoolman=Pull pending=7 endless=true ttg=1,0,2,3
  dryer=true/true/41.5/55.0/120/240/30 gatedry=
  g0 Available #FF0000 mat='PLA' name='Red' brand='' spool=12 temp=210-210 grp=0 err=- sensor=true/true
  g1 Loaded #00FF00 mat='PETG' name='Green PETG' brand='' spool=0 temp=230-230 grp=0 err=- sensor=true/false
  g2 Empty #FF8000 mat='ABS' name='Orange' brand='' spool=0 temp=250-250 grp=1 err=- sensor=true/false
  g3 Unknown #000000 mat='' name='Blue' brand='' spool=5 temp=200-200 grp=1 err=- sensor=false/false
  events=1 toasts='Same spool in Gate 3? Tap Clear if it is a new one.'
load: loaded
slot=2 tool=2 loaded=true action=Idle detail='' bypass=true units=1 hub=true slot_sensors=true
  pos=8 bowden=-1 unit=0 counts=4 seg=None fault='' rawgates=1,2,0,-1
  espooler='assist' fb='neutral' bias=0.250/-0.500 drive=true clog=2 enc=true/97/8.0/20.0/15.0/6.0 fg=true/false//0.10/0.80/-0.80 fgmode=1 led='gate_status' flow=99.5 purge=45.0
  tc=2/10 spoolman=Pull pending=7 endless=true ttg=1,0,2,3
  dryer=true/true/41.5/55.0/120/240/30 gatedry=
  g0 Available #FF0000 mat='PLA' name='Red' brand='' spool=12 temp=210-210 grp=0 err=- sensor=true/true
  g1 From Buffer #00FF00 mat='PETG' name='Green PETG' brand='' spool=0 temp=230-230 grp=0 err=- sensor=true/false
  g2 Empty #FF8000 mat='ABS' name='Orange' brand='' spool=0 temp=250-250 grp=1 err=- sensor=true/false
  g3 Unknown #000000 mat='' name='Blue' brand='' spool=5 temp=200-200 grp=1 err=- sensor=false/false
  events=1 toasts=''
unload: unloading
slot=2 tool=2 loaded=true action=Unloading detail='' bypass=true units=1 hub=true slot_sensors=true
  pos=4 bowden=-1 unit=0 counts=4 seg=None fault='' rawgates=1,2,0,-1
  espooler='assist' fb='neutral' bias=0.250/-0.500 drive=true clog=2 enc=true/97/8.0/20.0/15.0/6.0 fg=true/false//0.10/0.80/-0.80 fgmode=1 led='gate_status' flow=99.5 purge=45.0
  tc=2/10 spoolman=Pull pending=7 endless=true ttg=1,0,2,3
  dryer=true/true/41.5/55.0/120/240/30 gatedry=
  g0 Available #FF0000 mat='PLA' name='Red' brand='' spool=12 temp=210-210 grp=0 err=- sensor=true/true
  g1 From Buffer #00FF00 mat='PETG' name='Green PETG' brand='' spool=0 temp=230-230 grp=0 err=- sensor=true/false
  g2 Empty #FF8000 mat='ABS' name='Orange' brand='' spool=0 temp=250-250 grp=1 err=- sensor=true/false
  g3 Unknown #000000 mat='' name='Blue' brand='' spool=5 temp=200-200 grp=1 err=- sensor=false/false
  events=1 toasts=''
unload: unloaded
slot=-1 tool=-1 loaded=false action=Idle detail='' bypass=true units=1 hub=false slot_sensors=true
  pos=0 bowden=-1 unit=0 counts=4 seg=None fault='' rawgates=1,2,0,-1
  espooler='assist' fb='neutral' bias=0.250/-0.500 drive=true clog=2 enc=true/97/8.0/20.0/15.0/6.0 fg=true/false//0.10/0.80/-0.80 fgmode=1 led='gate_status' flow=99.5 purge=45.0
  tc=2/10 spoolman=Pull pending=7 endless=true ttg=1,0,2,3
  dryer=true/true/41.5/55.0/120/240/30 gatedry=
  g0 Available #FF0000 mat='PLA' name='Red' brand='' spool=12 temp=210-210 grp=0 err=- sensor=true/true
  g1 From Buffer #00FF00 mat='PETG' name='Green PETG' brand='' spool=0 temp=230-230 grp=0 err=- sensor=true/false
  g2 Empty #FF8000 mat='ABS' name='Orange' brand='' spool=0 temp=250-250 grp=1 err=- sensor=true/false
  g3 Unknown #000000 mat='' name='Blue' brand='' spool=5 temp=200-200 grp=1 err=- sensor=false/false
  events=1 toasts=''
fault: paused
slot=1 tool=1 loaded=false action=Idle detail='' bypass=true units=1 hub=true slot_sensors=true
  pos=5 bowden=-1 unit=0 counts=4 seg=Output Tube fault='Filament is stuck in the extruder' rawgates=1,2,0,-1
  espooler='assist' fb='neutral' bias=0.250/-0.500 drive=true clog=2 enc=true/97/8.0/20.0/15.0/6.0 fg=true/false//0.10/0.80/-0.80 fgmode=1 led='gate_status' flow=99.5 purge=45.0
  tc=2/10 spoolman=Pull pending=7 endless=true ttg=1,0,2,3
  dryer=true/true/41.5/55.0/120/240/30 gatedry=
  g0 Available #FF0000 mat='PLA' name='Red' brand='' spool=12 temp=210-210 grp=0 err=- sensor=true/true
  g1 From Buffer #00FF00 mat='PETG' name='Green PETG' brand='' spool=0 temp=230-230 grp=0 err=Filament is stuck in the extruder sensor=true/false
  g2 Empty #FF8000 mat='ABS' name='Orange' brand='' spool=0 temp=250-250 grp=1 err=- sensor=true/false
  g3 Unknown #000000 mat='' name='Blue' brand='' spool=5 temp=200-200 grp=1 err=- sensor=false/false
  events=1 toasts=''
fault: resumed
slot=1 tool=1 loaded=false action=Idle detail='' bypass=true units=1 hub=true slot_sensors=true
  pos=5 bowden=-1 unit=0 counts=4 seg=None fault='' rawgates=1,2,0,-1
  espooler='assist' fb='neutral' bias=0.250/-0.500 drive=true clog=2 enc=true/97/8.0/20.0/15.0/6.0 fg=true/false//0.10/0.80/-0.80 fgmode=1 led='gate_status' flow=99.5 purge=45.0
  tc=2/10 spoolman=Pull pending=7 endless=true ttg=1,0,2,3
  dryer=true/true/41.5/55.0/120/240/30 gatedry=
  g0 Available #FF0000 mat='PLA' name='Red' brand='' spool=12 temp=210-210 grp=0 err=- sensor=true/true
  g1 From Buffer #00FF00 mat='PETG' name='Green PETG' brand='' spool=0 temp=230-230 grp=0 err=- sensor=true/false
  g2 Empty #FF8000 mat='ABS' name='Orange' brand='' spool=0 temp=250-250 grp=1 err=- sensor=true/false
  g3 Unknown #000000 mat='' name='Blue' brand='' spool=5 temp=200-200 grp=1 err=- sensor=false/false
  events=1 toasts=''
insert: gate 3 fills
slot=1 tool=1 loaded=false action=Idle detail='' bypass=true units=1 hub=true slot_sensors=true
  pos=5 bowden=-1 unit=0 counts=4 seg=None fault='' rawgates=1,2,0,1
  espooler='assist' fb='neutral' bias=0.250/-0.500 drive=true clog=2 enc=true/97/8.0/20.0/15.0/6.0 fg=true/false//0.10/0.80/-0.80 fgmode=1 led='gate_status' flow=99.5 purge=45.0
  tc=2/10 spoolman=Pull pending=7 endless=true ttg=1,0,2,3
  dryer=true/true/41.5/55.0/120/240/30 gatedry=
  g0 Available #FF0000 mat='PLA' name='Red' brand='' spool=12 temp=210-210 grp=0 err=- sensor=true/true
  g1 From Buffer #00FF00 mat='PETG' name='Green PETG' brand='' spool=0 temp=230-230 grp=0 err=- sensor=true/false
  g2 Empty #FF8000 mat='ABS' name='Orange' brand='' spool=0 temp=250-250 grp=1 err=- sensor=true/false
  g3 Available #000000 mat='' name='Blue' brand='' spool=5 temp=200-200 grp=1 err=- sensor=false/false
  events=1 toasts=''
insert: gate 2 fills
slot=1 tool=1 loaded=false action=Idle detail='' bypass=true units=1 hub=true slot_sensors=true
  pos=5 bowden=-1 unit=0 counts=4 seg=None fault='' rawgates=1,2,1,1
  espooler='assist' fb='neutral' bias=0.250/-0.500 drive=true clog=2 enc=true/97/8.0/20.0/15.0/6.0 fg=true/false//0.10/0.80/-0.80 fgmode=1 led='gate_status' flow=99.5 purge=45.0
  tc=2/10 spoolman=Pull pending=7 endless=true ttg=1,0,2,3
  dryer=true/true/41.5/55.0/120/240/30 gatedry=
  g0 Available #FF0000 mat='PLA' name='Red' brand='' spool=12 temp=210-210 grp=0 err=- sensor=true/true
  g1 From Buffer #00FF00 mat='PETG' name='Green PETG' brand='' spool=0 temp=230-230 grp=0 err=- sensor=true/false
  g2 Available #FF8000 mat='ABS' name='Orange' brand='' spool=0 temp=250-250 grp=1 err=- sensor=true/false
  g3 Available #000000 mat='' name='Blue' brand='' spool=5 temp=200-200 grp=1 err=- sensor=false/false
  events=1 toasts=''
hex colours only
slot=1 tool=1 loaded=false action=Idle detail='' bypass=true units=1 hub=true slot_sensors=true
  pos=5 bowden=-1 unit=0 counts=4 seg=None fault='' rawgates=1,2,1,1
  espooler='assist' fb='neutral' bias=0.250/-0.500 drive=true clog=2 enc=true/97/8.0/20.0/15.0/6.0 fg=true/false//0.10/0.80/-0.80 fgmode=1 led='gate_status' flow=99.5 purge=45.0
  tc=2/10 spoolman=Pull pending=7 endless=true ttg=1,0,2,3
  dryer=true/true/41.5/55.0/120/240/30 gatedry=
  g0 Available #FF8800 mat='PLA' name='Red' brand='' spool=12 temp=210-210 grp=0 err=- sensor=true/true
  g1 From Buffer #808080 mat='PETG' name='Green PETG' brand='' spool=0 temp=230-230 grp=0 err=- sensor=true/false
  g2 Available #00FF00 mat='ABS' name='Orange' brand='' spool=0 temp=250-250 grp=1 err=- sensor=true/false
  g3 Available #000000 mat='' name='Blue' brand='' spool=5 temp=200-200 grp=1 err=- sensor=false/false
  events=1 toasts=''
temperature + name deltas
slot=1 tool=1 loaded=false action=Idle detail='' bypass=true units=1 hub=true slot_sensors=true
  pos=5 bowden=-1 unit=0 counts=4 seg=None fault='' rawgates=1,2,1,1
  espooler='assist' fb='neutral' bias=0.250/-0.500 drive=true clog=2 enc=true/97/8.0/20.0/15.0/6.0 fg=true/false//0.10/0.80/-0.80 fgmode=1 led='gate_status' flow=99.5 purge=45.0
  tc=2/10 spoolman=Pull pending=7 endless=true ttg=1,0,2,3
  dryer=true/true/41.5/55.0/120/240/30 gatedry=
  g0 Available #FF8800 mat='PLA' name='Crimson' brand='' spool=12 temp=215-215 grp=0 err=- sensor=true/true
  g1 From Buffer #808080 mat='PETG' name='Green PETG' brand='' spool=0 temp=230-230 grp=0 err=- sensor=true/false
  g2 Available #00FF00 mat='ABS' name='Orange' brand='' spool=0 temp=250-250 grp=1 err=- sensor=true/false
  g3 Available #000000 mat='' name='Blue' brand='' spool=5 temp=200-200 grp=1 err=- sensor=false/false
  events=1 toasts=''
per-gate drying array
slot=1 tool=1 loaded=false action=Idle detail='' bypass=true units=1 hub=true slot_sensors=true
  pos=5 bowden=-1 unit=0 counts=4 seg=None fault='' rawgates=1,2,1,1
  espooler='assist' fb='neutral' bias=0.250/-0.500 drive=true clog=2 enc=true/97/8.0/20.0/15.0/6.0 fg=true/false//0.10/0.80/-0.80 fgmode=1 led='gate_status' flow=99.5 purge=45.0
  tc=2/10 spoolman=Pull pending=7 endless=true ttg=1,0,2,3
  dryer=true/true/41.5/55.0/120/240/30 gatedry=active||queued||
  g0 Available #FF8800 mat='PLA' name='Crimson' brand='' spool=12 temp=215-215 grp=0 err=- sensor=true/true
  g1 From Buffer #808080 mat='PETG' name='Green PETG' brand='' spool=0 temp=230-230 grp=0 err=- sensor=true/false
  g2 Available #00FF00 mat='ABS' name='Orange' brand='' spool=0 temp=250-250 grp=1 err=- sensor=true/false
  g3 Available #000000 mat='' name='Blue' brand='' spool=5 temp=200-200 grp=1 err=- sensor=false/false
  events=1 toasts=''
aggregate sensors
slot=3 tool=1 loaded=false action=Idle detail='' bypass=true units=1 hub=true slot_sensors=true
  pos=5 bowden=-1 unit=0 counts=4 seg=None fault='' rawgates=1,2,1,1
  espooler='assist' fb='neutral' bias=0.250/-0.500 drive=true clog=2 enc=true/97/8.0/20.0/15.0/6.0 fg=true/false//0.10/0.80/-0.80 fgmode=1 led='gate_status' flow=99.5 purge=45.0
  tc=2/10 spoolman=Pull pending=7 endless=true ttg=1,0,2,3
  dryer=true/true/41.5/55.0/120/240/30 gatedry=active||queued||
  g0 Available #FF8800 mat='PLA' name='Crimson' brand='' spool=12 temp=215-215 grp=0 err=- sensor=true/false
  g1 From Buffer #808080 mat='PETG' name='Green PETG' brand='' spool=0 temp=230-230 grp=0 err=- sensor=true/false
  g2 Available #00FF00 mat='ABS' name='Orange' brand='' spool=0 temp=250-250 grp=1 err=- sensor=true/false
  g3 Available #000000 mat='' name='Blue' brand='' spool=5 temp=200-200 grp=1 err=- sensor=true/true
  events=1 toasts=''
topology string
slot=3 tool=1 loaded=false action=Idle detail='' bypass=true units=2 hub=true slot_sensors=true
  pos=5 bowden=-1 unit=1 counts=6,4 seg=None fault='' rawgates=1,2,1,1
  espooler='assist' fb='neutral' bias=0.250/-0.500 drive=true clog=2 enc=true/97/8.0/20.0/15.0/6.0 fg=true/false//0.10/0.80/-0.80 fgmode=1 led='gate_status' flow=99.5 purge=45.0
  tc=2/10 spoolman=Pull pending=7 endless=true ttg=1,0,2,3
  dryer=true/true/41.5/55.0/120/240/30 gatedry=active||queued||
  g0 Available #FF8800 mat='PLA' name='Crimson' brand='' spool=12 temp=215-215 grp=0 err=- sensor=true/false
  g1 From Buffer #808080 mat='PETG' name='Green PETG' brand='' spool=0 temp=230-230 grp=0 err=- sensor=true/false
  g2 Available #00FF00 mat='ABS' name='Orange' brand='' spool=0 temp=250-250 grp=1 err=- sensor=true/false
  g3 Available #000000 mat='' name='Blue' brand='' spool=5 temp=200-200 grp=1 err=- sensor=true/true
  events=1 toasts=''
idle sparse frame
slot=3 tool=1 loaded=false action=Idle detail='' bypass=true units=2 hub=true slot_sensors=true
  pos=7 bowden=-1 unit=1 counts=6,4 seg=None fault='' rawgates=1,2,1,1
  espooler='assist' fb='neutral' bias=0.250/-0.500 drive=true clog=2 enc=true/97/8.0/20.0/15.0/6.0 fg=true/false//0.10/0.80/-0.80 fgmode=1 led='gate_status' flow=99.5 purge=45.0
  tc=2/10 spoolman=Pull pending=7 endless=true ttg=1,0,2,3
  dryer=true/true/41.5/55.0/120/240/30 gatedry=active||queued||
  g0 Available #FF8800 mat='PLA' name='Crimson' brand='' spool=12 temp=215-215 grp=0 err=- sensor=true/false
  g1 From Buffer #808080 mat='PETG' name='Green PETG' brand='' spool=0 temp=230-230 grp=0 err=- sensor=true/false
  g2 Available #00FF00 mat='ABS' name='Orange' brand='' spool=0 temp=250-250 grp=1 err=- sensor=true/false
  g3 Available #000000 mat='' name='Blue' brand='' spool=5 temp=200-200 grp=1 err=- sensor=true/true
  events=1 toasts=''
)GOLD";

inline void connect_with_fixture(AmsBackendHappyHareTestHelper& helper,
                                 helix::test::FakeMoonrakerClient& client,
                                 const nlohmann::json& fx) {
    HappyHareTestAccess::on_started(helper);
    REQUIRE(client.reply_to_last(
        kObjectsQuery, nlohmann::json{{"result",
                                       {{"status",
                                         {{"configfile", {{"settings", fx["configfile_settings"]}}},
                                          {"mmu_machine", fx.contains("mmu_machine")
                                                              ? fx["mmu_machine"]
                                                              : nlohmann::json::object()}}}}}}));
    helix::ui::UpdateQueue::instance().drain();
}

/// One status notification carrying @p params as the printer sends them:
/// printer.mmu beside whatever sibling objects changed.
inline void feed_params(AmsBackendHappyHareTestHelper& helper, const nlohmann::json& params) {
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    HappyHareTestAccess::handle_status_update(helper, notification);
}

inline std::vector<std::string> unit_scoped_commands(AmsBackendHappyHareTestHelper& helper,
                                                     int drying_unit) {
    helper.set_running(true);
    HappyHareTestAccess::dryer_info(helper).supported = true;
    helper.clear_captured_gcodes();
    CHECK(helper.reset().success());
    helper.execute_device_action("motors_toggle", std::any(true));
    helper.execute_device_action("motors_toggle", std::any(false));
    CHECK(helper.start_drying(50.0f, 60, -1, drying_unit).success());
    CHECK(helper.update_drying(55.0f, -1, -1, drying_unit).success());
    CHECK(helper.stop_drying(drying_unit).success());
    helper.execute_device_action("servo_up", {});
    helper.execute_device_action("calibrate_gates", {});
    helper.execute_device_action("calibrate_bowden", {});
    helper.execute_device_action("test_grip", {});
    helper.execute_device_action("gear_from_spool_speed", std::any(70.0));
    helper.execute_device_action("extruder_load_speed", std::any(20.0));
    helper.execute_device_action("toolhead_ooze_reduction", std::any(1.0));
    helper.execute_device_action("sync_to_extruder", std::any(true));
    return helper.captured_gcodes;
}

inline bool action_enabled(const AmsBackendHappyHareTestHelper& helper, const std::string& id) {
    for (const auto& a : helper.get_device_actions()) {
        if (a.id == id) {
            return a.enabled;
        }
    }
    FAIL("no device action " << id);
    return false;
}

class ScopedConfig {
  public:
    ScopedConfig()
        : config_(helix::Config::get_instance()), data_(helix::ConfigTestAccess::data(*config_)),
          path_(helix::ConfigTestAccess::path(*config_)) {
        helix::ConfigTestAccess::path(*config_).clear(); // save() skips an empty path
    }
    ~ScopedConfig() {
        helix::ConfigTestAccess::data(*config_) = data_;
        helix::ConfigTestAccess::path(*config_) = path_;
    }
    ScopedConfig(const ScopedConfig&) = delete;
    ScopedConfig& operator=(const ScopedConfig&) = delete;

    helix::Config* operator->() const {
        return config_;
    }

  private:
    helix::Config* config_;
    nlohmann::json data_;
    std::string path_;
};

struct TuningRig {
    MockPrinter printer;
    helix::test::FakeMoonrakerClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> reg;
    explicit TuningRig(const nlohmann::json& tuning = nullptr) : reg(&printer.api, &client) {
        if (!tuning.is_null()) {
            printer.client.mock_db_set("helix-screen", "hh_tuning", tuning);
        }
        reg->initialize_test_gates(4);
    }
    void connect(const nlohmann::json& fx =
                     helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json")) {
        connect_with_fixture(*reg, client, fx);
        for (int i = 0; i < 3; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }
    [[nodiscard]] nlohmann::json stored() {
        return printer.client.mock_db_get("helix-screen", "hh_tuning");
    }
};

/// A v3 Happy Hare with a moving selector, which takes every tunable.
inline nlohmann::json v3_linear_fixture() {
    return {{"configfile_settings",
             {{"mmu", {{"happy_hare_version", 3.42}, {"selector_move_speed", 200.0}}},
              {"mmu_machine", {{"selector_type", "LinearSelector"}}}}}};
}

inline nlohmann::json v3_fixture(double version, const std::string& selector = "LinearSelector") {
    return {{"configfile_settings",
             {{"mmu",
               {{"happy_hare_version", version},
                {"enable_clog_detection", 1},
                {"flowguard_encoder_mode", 1},
                {"flowguard_encoder_max_motion", 18.0}}},
              {"mmu_machine", {{"selector_type", selector}}}}}};
}

inline std::map<std::string, helix::printer::DeviceAction>
v4_control_rows(const AmsBackendHappyHareTestHelper& helper) {
    std::map<std::string, helix::printer::DeviceAction> rows;
    for (auto& a : helper.get_device_actions()) {
        if (a.id.rfind("sync_feedback", 0) == 0 || a.id.rfind("mmu_fan_", 0) == 0) {
            rows.emplace(a.id, a);
        }
    }
    return rows;
}

inline nlohmann::json fan_entry(const char* unit, int first_gate, bool enabled,
                                const nlohmann::json& modes) {
    return {{"unit", unit},
            {"first_gate", first_gate},
            {"per_gate", modes.size() > 1},
            {"enabled", enabled},
            {"modes", modes}};
}

inline nlohmann::json hh_gates(const std::vector<int>& gate_status, int gate,
                               const char* filament) {
    return nlohmann::json{{"gate_status", gate_status}, {"gate", gate}, {"filament", filament}};
}

} // namespace helix::test
