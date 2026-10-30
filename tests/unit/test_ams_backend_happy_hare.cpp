// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_device_section_detail_overlay.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/filament_accounting_cases.h"
#include "../ui_test_utils.h"
#include "action_prompt_manager.h"
#include "ams_backend_happy_hare.h"
#include "ams_state.h"
#include "ams_types.h"
#include "buffer_reading.h"
#include "config.h"
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
#include "test_helpers/gcode_recording_api.h"
#include "test_helpers/happy_hare_fixture.h"
#include "test_helpers/happy_hare_test_access.h"
#include "test_helpers/print_state_test_drivers.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/seeded_override.h"

#include <algorithm>
#include <any>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

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
using helix::AmsBackendHappyHareTestHelper;

namespace {
const helix::printer::DeviceAction*
find_action(const std::vector<helix::printer::DeviceAction>& all, const std::string& id) {
    for (const auto& a : all) {
        if (a.id == id) {
            return &a;
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE("Happy Hare persistence: an edit writes exactly the fields it changed",
          "[ams][happy_hare][persistence][1597]") {
    // kKeep leaves a field at its SlotInfo default. Grey is the "no colour"
    // sentinel and is never written; pure black is a deliberate pick and is.
    constexpr int kKeep = -99;
    struct Row {
        const char* name;
        int gate;
        int prior_spool;
        int color;
        const char* material;
        int spool;
        int mapped_tool;
        const char* exact;    // a command that must be sent verbatim ("" = none)
        const char* contains; // a fragment some command must carry ("" = none)
        const char* absent;   // a fragment no command may carry ("" = none)
        bool sends_nothing;
        int mapping_after; // gate's mapped_tool afterwards (kKeep = not checked)
    };
    const Row rows[] = {
        {"colour, uppercase hex without #", 0, 0, 0xFF0000, nullptr, kKeep, kKeep,
         "MMU_GATE_MAP GATE=0 COLOR=FF0000", "", "", false, kKeep},
        {"colour on another gate", 1, 0, 0x00FF00, nullptr, kKeep, kKeep,
         "MMU_GATE_MAP GATE=1 COLOR=00FF00", "", "", false, kKeep},
        {"material alone", 1, 0, kKeep, "PLA", kKeep, kKeep, "MMU_GATE_MAP GATE=1 MATERIAL=PLA", "",
         "", false, kKeep},
        {"Spoolman id", 2, 0, kKeep, nullptr, 42, kKeep, "", "SPOOLID=42", "", false, kKeep},
        {"unlinking a spool sends -1", 0, 123, kKeep, nullptr, 0, kKeep, "", "SPOOLID=-1", "",
         false, kKeep},
        {"every field", 0, 0, 0x0000FF, "PETG", 99, kKeep,
         "MMU_GATE_MAP GATE=0 COLOR=0000FF MATERIAL=PETG SPOOLID=99", "", "", false, kKeep},
        {"default grey is not a colour", 0, 0, 0x808080, "PLA", kKeep, kKeep, "", "",
         "COLOR=", false, kKeep},
        {"pure black is a colour", 0, 0, 0x000000, "ABS", kKeep, kKeep,
         "MMU_GATE_MAP GATE=0 COLOR=000000 MATERIAL=ABS", "", "", false, kKeep},
        {"empty material is not written", 0, 0, 0xFF0000, "", kKeep, kKeep, "", "",
         "MATERIAL=", false, kKeep},
        {"no spool before or after", 0, 0, 0xFF0000, nullptr, 0, kKeep, "", "", "SPOOLID=", false,
         kKeep},
        {"nothing set sends nothing", 0, 0, 0x808080, "", 0, kKeep, "", "", "", true, kKeep},
        {"gate 3", 3, 0, 0xFF0000, nullptr, kKeep, kKeep, "", "MMU_GATE_MAP GATE=3", "", false,
         kKeep},
        {"gate 7", 7, 0, 0xFF0000, nullptr, kKeep, kKeep, "", "MMU_GATE_MAP GATE=7", "", false,
         kKeep},
        {"a tool remap sends MMU_TTG_MAP", 0, 0, kKeep, nullptr, kKeep, 2,
         "MMU_TTG_MAP TOOL=2 GATE=0", "", "", false, kKeep},
        {"the same tool sends no MMU_TTG_MAP", 0, 0, kKeep, "PLA", kKeep, 0, "", "", "MMU_TTG_MAP ",
         false, kKeep},
        {"a default -1 tool leaves the mapping", 2, 0, kKeep, "PLA", kKeep, kKeep, "", "",
         "MMU_TTG_MAP ", false, 2},
    };

    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(8);
        if (row.prior_spool != 0) {
            helper.get_mutable_slot(row.gate)->spoolman_id = row.prior_spool;
        }

        SlotInfo info;
        if (row.color != kKeep)
            info.color_rgb = static_cast<uint32_t>(row.color);
        if (row.material)
            info.material = row.material;
        if (row.spool != kKeep)
            info.spoolman_id = row.spool;
        if (row.mapped_tool != kKeep)
            info.mapped_tool = row.mapped_tool;
        helix::test::apply_edit(helper, row.gate, info);

        if (row.sends_nothing)
            CHECK(helper.captured_gcodes.empty());
        if (*row.exact)
            CHECK(helper.has_gcode(row.exact));
        if (*row.contains)
            CHECK(helper.has_gcode_containing(row.contains));
        if (*row.absent)
            CHECK_FALSE(helper.has_gcode_containing(row.absent));
        if (row.mapping_after != kKeep)
            CHECK(helper.get_slot_entry(row.gate)->info.mapped_tool == row.mapping_after);
    }
}

TEST_CASE("Happy Hare sync_external_identity carries the whole Spoolman link",
          "[ams][happy_hare]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    SlotInfo info;
    info.spoolman_id = 42;
    info.spoolman_filament_id = 77;
    info.spoolman_vendor_id = 3;
    helper.sync_external_identity(0, info);

    const SlotInfo stored = helper.get_slot_info(0);
    CHECK(stored.spoolman_filament_id == 77);
    CHECK(stored.spoolman_vendor_id == 3);
}
TEST_CASE("Happy Hare persistence: clearing one field names it empty",
          "[ams][happy_hare][persistence]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);

    SlotInfo* slot = helper.get_mutable_slot(0);
    REQUIRE(slot != nullptr);
    slot->material = "PLA";
    slot->color_rgb = 0xED2C2C;
    SlotInfo* second = helper.get_mutable_slot(1);
    REQUIRE(second != nullptr);
    second->material = "PLA";
    second->color_rgb = 0xED2C2C;

    // The editor saves the whole slot, so the clear is an edit that keeps the
    // colour and empties the material. Happy Hare keeps an omitted parameter
    // at its current value, so the write must name the emptied field with an
    // explicit empty or the gate map goes on remembering PLA.
    SlotInfo cleared_material = *slot;
    cleared_material.material.clear();
    helix::test::apply_edit(helper, 0, cleared_material);
    REQUIRE(helper.has_gcode("MMU_GATE_MAP GATE=0 COLOR=ED2C2C MATERIAL="));

    // The same edit on the colour, keeping the material.
    SlotInfo cleared_color = *second;
    cleared_color.color_rgb = helix::kAmsDefaultSlotColor;
    helix::test::apply_edit(helper, 1, cleared_color);
    REQUIRE(helper.has_gcode("MMU_GATE_MAP GATE=1 COLOR= MATERIAL=PLA"));

    // Neither took the full wipe: a single-field clear keeps every other
    // field the gate map holds.
    REQUIRE_FALSE(helper.has_gcode_containing("SPOOLID=-1"));
    REQUIRE_FALSE(helper.has_gcode_containing("NAME="));
}

// ============================================================================
// Clear Spool - the full gate-map wipe
// ============================================================================
//
// The Clear Spool funnel (ui_ams_detail CLEAR_SPOOL) hands the backend a copy
// of the slot with every identity field blanked. Happy Hare keeps omitted
// params at their current value, so the clear must name each writable field
// with an explicit empty value: that is the only form that empties MATERIAL,
// COLOR, NAME and VENDOR on every HH version.
// ============================================================================

/// The SlotInfo ui_ams_detail's CLEAR_SPOOL arm hands commit_slot_edit(): a
/// copy of the live slot with every identity field blanked and the Spoolman
/// link dropped.
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

TEST_CASE("Happy Hare clear: the wipe names every gate-map field", "[ams][happy_hare][clear]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);

    SlotInfo* slot = helper.get_mutable_slot(0);
    REQUIRE(slot != nullptr);
    slot->material = "PLA";
    slot->color_rgb = 0xFF0000;
    slot->brand = "Prusa";
    slot->spool_name = "Galaxy Black";
    slot->spoolman_id = 123;

    helix::test::apply_edit(helper, 0, funnel_cleared_copy(*slot));

    // Each field named with an explicit empty value (v2/v3 ignore params they
    // do not fetch; VENDOR is v4-only), SPOOLID=-1 unlinks, QUIET keeps the
    // echo out of the printer console.
    REQUIRE(
        helper.has_gcode("MMU_GATE_MAP GATE=0 MATERIAL= COLOR= NAME= VENDOR= SPOOLID=-1 QUIET=1"));
    // RESET ignores GATE on v2/v3 and wipes every gate; TEMP=0 is "keep the
    // current value"; AVAILABLE=0 marks the gate EMPTY, not unknown.
    REQUIRE_FALSE(helper.has_gcode_containing("RESET="));
    REQUIRE_FALSE(helper.has_gcode_containing("TEMP="));
    REQUIRE_FALSE(helper.has_gcode_containing("AVAILABLE="));
}

TEST_CASE("Happy Hare clear: a material edit does not take the wipe path",
          "[ams][happy_hare][clear]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);

    SlotInfo info;
    info.material = "PLA";

    helix::test::apply_edit(helper, 0, info);

    // An editor commit carries the whole slot, so a kept material arrives
    // non-empty and stays on the incremental path.
    REQUIRE(helper.has_gcode("MMU_GATE_MAP GATE=0 MATERIAL=PLA"));
    REQUIRE_FALSE(helper.has_gcode_containing("SPOOLID=-1"));
    REQUIRE_FALSE(helper.has_gcode_containing("QUIET=1"));
}

TEST_CASE("Happy Hare clear: pull mode skips the firmware write and says so",
          "[ams][happy_hare][clear]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);

    helper.test_parse_mmu_state(nlohmann::json{{"spoolman_support", "pull"}});
    REQUIRE(helper.get_system_info().spoolman_mode == SpoolmanMode::PULL);

    SlotInfo* slot = helper.get_mutable_slot(0);
    REQUIRE(slot != nullptr);
    slot->material = "PLA";
    slot->spoolman_id = 123;

    const AmsError err = helix::test::apply_edit(helper, 0, funnel_cleared_copy(*slot));

    // Happy Hare refuses local writes to material/colour/name/vendor/spool id
    // in pull mode, and the refusal is logged there rather than returned, so
    // nothing is sent and the failure is reported here instead.
    REQUIRE_FALSE(helper.has_gcode_containing("MMU_GATE_MAP"));
    REQUIRE_FALSE(err.success());
    REQUIRE(err.partially_applied);
    // Our own layer is cleared even though the firmware refused.
    const SlotInfo* after = helper.get_mutable_slot(0);
    REQUIRE(after->material.empty());
    REQUIRE(after->spoolman_id == 0);
}

TEST_CASE("Happy Hare clear: slot stays blank across the status echo", "[ams][happy_hare][clear]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);

    SlotInfo identity;
    identity.material = "PLA";
    identity.color_rgb = 0xFF0000;
    identity.spoolman_id = 42;
    REQUIRE(helix::test::apply_edit(helper, 0, identity).success());
    helper.clear_captured_gcodes();

    REQUIRE(helix::test::apply_edit(helper, 0, funnel_cleared_copy(*helper.get_mutable_slot(0)))
                .success());
    REQUIRE(
        helper.has_gcode("MMU_GATE_MAP GATE=0 MATERIAL= COLOR= NAME= VENDOR= SPOOLID=-1 QUIET=1"));

    // Live: blank.
    const SlotInfo* live = helper.get_mutable_slot(0);
    REQUIRE(live->material.empty());
    REQUIRE(live->color_rgb == helix::kAmsDefaultSlotColor);
    REQUIRE(live->spoolman_id == 0);

    // The firmware's echo of the cleared gate map: still blank. The cleared
    // override record must not repaint anything onto it.
    nlohmann::json echo = {
        {"gate_material", {"", ""}}, {"gate_color", {"", ""}}, {"gate_spool_id", {0, 0}}};
    helper.test_parse_mmu_state(echo);
    const SlotInfo* echoed = helper.get_mutable_slot(0);
    REQUIRE(echoed->material.empty());
    REQUIRE(echoed->color_rgb == helix::kAmsDefaultSlotColor);
    REQUIRE(echoed->spoolman_id == 0);
}

TEST_CASE("Happy Hare editor write: pull mode skips the firmware write and says so",
          "[ams][happy_hare]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);

    helper.test_parse_mmu_state(nlohmann::json{{"spoolman_support", "pull"}});
    REQUIRE(helper.get_system_info().spoolman_mode == SpoolmanMode::PULL);

    SlotInfo edit;
    edit.material = "PETG";
    edit.mapped_tool = 1;
    const AmsError err = helix::test::apply_edit(helper, 0, edit);

    // Same refusal as the clear: Happy Hare ignores local writes to the
    // gate-map fields in pull mode and only logs it, so nothing is sent and
    // the failure is reported here instead of a success.
    REQUIRE_FALSE(helper.has_gcode_containing("MMU_GATE_MAP"));
    REQUIRE_FALSE(err.success());
    REQUIRE(err.partially_applied);
    // A tool remap is not a gate-map field: Happy Hare takes it in pull mode.
    REQUIRE(helper.has_gcode_containing("MMU_TTG_MAP TOOL=1 GATE=0"));
    // Our own layer keeps the edit even though the firmware refused.
    const SlotInfo* after = helper.get_mutable_slot(0);
    REQUIRE(after->material == "PETG");
    REQUIRE(after->mapped_tool == 1);
}

TEST_CASE("Happy Hare editor write: pull mode still saves a pure tool remap", "[ams][happy_hare]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);

    helper.test_parse_mmu_state(nlohmann::json{{"spoolman_support", "pull"}});
    REQUIRE(helper.get_system_info().spoolman_mode == SpoolmanMode::PULL);

    SlotInfo edit;
    edit.mapped_tool = 1;
    const AmsError err = helix::test::apply_edit(helper, 0, edit);

    // No gate-map field changed, so the pull-mode refusal must not fire.
    REQUIRE(err.success());
    REQUIRE(helper.has_gcode_containing("MMU_TTG_MAP TOOL=1 GATE=0"));
    REQUIRE_FALSE(helper.has_gcode_containing("MMU_GATE_MAP"));
}

namespace {
/// A backend wired to a real PrinterState whose wire can be driven to
/// PRINTING - the mid-print guard reads api_->printer_state(), which needs a
/// non-null api.
struct PrintLifecycleFixture : public LVGLTestFixture {
    PrintLifecycleFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
    }

    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
};
} // namespace

TEST_CASE("Happy Hare clear mid-print: the active gate's firmware write is skipped",
          "[ams][happy_hare][clear]") {
    PrintLifecycleFixture fx;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(fx.api.get(),
                                                                             &fx.mock_client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);
    helper.set_current_slot(0);
    helix::test::set_wire_state(fx.state, helix::PrintJobState::PRINTING);
    helix::ui::UpdateQueue::instance().drain();

    for (int i : {0, 1}) {
        SlotInfo* slot = helper.get_mutable_slot(i);
        REQUIRE(slot != nullptr);
        slot->material = "PLA";
        slot->spoolman_id = 100 + i;
    }

    // The gate feeding the print: local layer cleared, firmware untouched.
    const AmsError active =
        helix::test::apply_edit(helper, 0, funnel_cleared_copy(*helper.get_mutable_slot(0)));
    REQUIRE_FALSE(active.success());
    REQUIRE(active.partially_applied);
    REQUIRE_FALSE(helper.has_gcode_containing("MMU_GATE_MAP"));
    REQUIRE(helper.get_mutable_slot(0)->material.empty());

    // Any other gate still clears normally.
    REQUIRE(helix::test::apply_edit(helper, 1, funnel_cleared_copy(*helper.get_mutable_slot(1)))
                .success());
    REQUIRE(
        helper.has_gcode("MMU_GATE_MAP GATE=1 MATERIAL= COLOR= NAME= VENDOR= SPOOLID=-1 QUIET=1"));
}

TEST_CASE("Happy Hare reset_tool_mappings sends one identity MMU_TTG_MAP per tool",
          "[ams][happy_hare][tool_mapping][reset]") {
    for (int gates : {0, 4, 8}) {
        INFO(gates << " gates");
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        if (gates > 0)
            helper.initialize_test_gates(gates);

        REQUIRE(helper.reset_tool_mappings().success());
        CHECK(helper.captured_gcodes.size() == static_cast<size_t>(gates));
        for (int t = 0; t < gates; ++t) {
            CHECK(helper.has_gcode("MMU_TTG_MAP TOOL=" + std::to_string(t) +
                                   " GATE=" + std::to_string(t)));
        }
    }
}

// ============================================================================
// Endless spool
// ============================================================================

TEST_CASE("Happy Hare endless spool edits on a single unit send MMU_ENDLESS_SPOOL",
          "[ams][happy_hare][endless_spool]") {
    using Op = std::function<AmsError(AmsBackendHappyHareTestHelper&)>;
    struct Row {
        const char* name;
        std::vector<int> groups; // preset endless_spool_group per gate; empty = ungrouped
        Op op;
        const char* gcode;
    };
    const Row rows[] = {
        // ENABLE=1 so HH does not skip RESET while endless spool is disabled.
        {"reset",
         {},
         [](auto& h) { return h.reset_endless_spool(); },
         "MMU_ENDLESS_SPOOL ENABLE=1 RESET=1 QUIET=1"},
        // Ungrouped gates take standalone ids 0,1,2,3; joining gate 0 to gate 2's
        // group gives 2,1,2,3. No ENABLE= on an edit: it would persist a state
        // change the user did not ask for.
        {"join gate 2's group",
         {},
         [](auto& h) { return h.set_endless_spool_backup(0, 2); },
         "MMU_ENDLESS_SPOOL QUIET=1 GROUPS=2,1,2,3"},
        // Removing a backup moves the gate to a fresh standalone group (max+1).
        {"remove a backup",
         {0, 0, 1, 1},
         [](auto& h) { return h.set_endless_spool_backup(0, -1); },
         "MMU_ENDLESS_SPOOL QUIET=1 GROUPS=2,0,1,1"},
        {"switch on",
         {},
         [](auto& h) { return h.set_endless_spool_enabled(true); },
         "MMU_ENDLESS_SPOOL ENABLE=1 QUIET=1"},
        {"switch off",
         {},
         [](auto& h) { return h.set_endless_spool_enabled(false); },
         "MMU_ENDLESS_SPOOL ENABLE=0 QUIET=1"},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        for (size_t i = 0; i < row.groups.size(); ++i)
            helper.get_mutable_slot(static_cast<int>(i))->endless_spool_group = row.groups[i];

        const auto caps = helper.get_endless_spool_capabilities();
        CHECK(caps.available());
        CHECK(caps.editable());
        CHECK(caps.switchable);
        CHECK(caps.editability == helix::printer::EndlessSpoolEditability::Group);

        CHECK(row.op(helper).success());
        CHECK(helper.has_gcode(row.gcode));
    }
}

TEST_CASE("Happy Hare endless spool is read-only on multi-unit",
          "[ams][happy_hare][endless_spool]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates_multi({4, 4});

    auto caps = helper.get_endless_spool_capabilities();
    CHECK(caps.available());
    CHECK_FALSE(caps.editable());
    CHECK_FALSE(caps.switchable);
    CHECK(caps.restriction == helix::printer::EndlessSpoolRestriction::MultiUnit);

    // MMU_ENDLESS_SPOOL has no UNIT= param, so every edit is refused on multi-unit.
    auto result = helper.set_endless_spool_backup(0, 1);
    CHECK_FALSE(result.success());
    CHECK(result.result == AmsResult::NOT_SUPPORTED);
    CHECK_FALSE(helper.set_endless_spool_enabled(true).success());
    CHECK(helper.captured_gcodes.empty());
}

// Bypass: Happy Hare's selector sits on the bypass position and reports gate -2.
// do_unload_filament() ignores the slot entirely and sends MMU_UNLOAD, so the
// sentinel needs no special handling here — but nothing pinned that, and the UI
// decision layer that DID refuse -2 was fixed by teaching it the sentinel is a
// real target. This asserts the backend half of that contract.
TEST_CASE("Happy Hare unload_filament under bypass sends MMU_UNLOAD", "[ams][happy_hare][bypass]") {
    MoonrakerClientMock client;
    helix::PrinterState state;
    helix::test::GcodeRecordingApi api{client, state};
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(&api);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.set_running(true);
    helper.set_filament_loaded(true);
    helper.set_current_slot(-2); // bypass sentinel

    auto result = helper.unload_filament(-2);

    REQUIRE(result.success());
    helix::ui::UpdateQueue::instance().drain(); // a pre-op G28 hands off on the queue
    REQUIRE(api.contains("MMU_UNLOAD"));
}

TEST_CASE("Happy Hare unload_filament under bypass still refuses an empty toolhead",
          "[ams][happy_hare][bypass]") {
    // Bypass being engaged is not itself evidence of filament: the selector can
    // sit on the bypass position with nothing fed. The backend's own guard must
    // still bite, so the UI's affordance and the backend's refusal agree.
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.set_running(true);
    helper.set_filament_loaded(false);
    helper.set_current_slot(-2);

    auto result = helper.unload_filament(-2);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::WRONG_STATE);
    REQUIRE_FALSE(helper.has_gcode_containing("MMU_UNLOAD"));
}

TEST_CASE("Happy Hare eject_lane and clear_fault address one gate or refuse",
          "[ams][happy_hare][eject][recovery][slot_index]") {
    // MMU_RECOVER is bookkeeping, not a filament move, so clear_fault is a
    // per-gate fault clear. -1 is the documented "no particular gate": bare
    // MMU_RECOVER re-syncs the whole selector. Zero gates means nothing is
    // discovered yet, which must not blame the index the user picked.
    enum class Op { Eject, ClearFault };
    struct Row {
        const char* name;
        Op op;
        int gates;
        bool running;
        int slot;
        AmsResult result;
        const char* gcode; // sent verbatim on success ("" = none)
    };
    const Row rows[] = {
        {"eject gate 0", Op::Eject, 4, true, 0, AmsResult::SUCCESS, "MMU_EJECT GATE=0"},
        {"eject gate 2", Op::Eject, 4, true, 2, AmsResult::SUCCESS, "MMU_EJECT GATE=2"},
        {"eject out of range", Op::Eject, 4, true, 99, AmsResult::INVALID_SLOT, ""},
        {"eject with no gates", Op::Eject, 0, true, 0, AmsResult::NOT_CONNECTED, ""},
        {"eject when stopped", Op::Eject, 4, false, 0, AmsResult::NOT_CONNECTED, ""},
        {"clear gate 2", Op::ClearFault, 4, true, 2, AmsResult::SUCCESS, "MMU_RECOVER GATE=2"},
        {"clear gate 3", Op::ClearFault, 4, true, 3, AmsResult::SUCCESS, "MMU_RECOVER GATE=3"},
        {"clear out of range", Op::ClearFault, 4, true, 99, AmsResult::INVALID_SLOT, ""},
        {"clear no particular gate", Op::ClearFault, 4, true, -1, AmsResult::SUCCESS,
         "MMU_RECOVER"},
        {"clear when stopped", Op::ClearFault, 4, false, 0, AmsResult::NOT_CONNECTED, ""},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(row.gates);
        helper.set_running(row.running);

        const AmsError err =
            row.op == Op::Eject ? helper.eject_lane(row.slot) : helper.clear_fault(row.slot);
        if (row.result == AmsResult::SUCCESS) {
            CHECK(err.success());
            CHECK(helper.has_gcode(row.gcode));
        } else {
            CHECK_FALSE(err.success());
            CHECK(err.result == row.result);
            CHECK(helper.captured_gcodes.empty());
        }
    }
}

// ============================================================================
// recover_with_state() Tests
// ============================================================================

namespace {
helix::RecoverStateRequest recover_req(int slot, bool bypass, std::optional<bool> loaded) {
    helix::RecoverStateRequest r;
    r.slot = slot;
    r.bypass = bypass;
    r.loaded = loaded;
    return r;
}
} // namespace

TEST_CASE("Happy Hare recover command names only what the user asserted",
          "[ams][happy_hare][recovery]") {
    struct Row {
        int slot;
        bool bypass;
        std::optional<bool> loaded;
        const char* expected;
    };
    // clang-format off
    const Row rows[] = {
        {-1, false, std::nullopt, "MMU_RECOVER"},
        {-1, false, false,        "MMU_RECOVER LOADED=0"},
        {-1, false, true,         "MMU_RECOVER LOADED=1"},
        { 1, false, std::nullopt, "MMU_RECOVER GATE=1"},
        { 1, false, false,        "MMU_RECOVER GATE=1 LOADED=0"},
        { 1, false, true,         "MMU_RECOVER GATE=1 LOADED=1"},
        { 0, false, std::nullopt, "MMU_RECOVER GATE=0"},
        // Bypass replaces the gate: HH forces tool and gate to bypass itself.
        {-1, true,  std::nullopt, "MMU_RECOVER BYPASS=1"},
        { 1, true,  false,        "MMU_RECOVER BYPASS=1 LOADED=0"},
        { 1, true,  true,         "MMU_RECOVER BYPASS=1 LOADED=1"},
    };
    // clang-format on
    for (const auto& row : rows) {
        INFO("slot=" << row.slot << " bypass=" << row.bypass);
        CHECK(helix::AmsBackendHappyHare::build_recover_command(
                  recover_req(row.slot, row.bypass, row.loaded)) == row.expected);
    }
}

TEST_CASE("Happy Hare recover_with_state sends the built command", "[ams][happy_hare][recovery]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.set_running(true);
    helper.set_supports_bypass(true);

    CHECK(helper.supports_recover_with_state());

    REQUIRE(helper.recover_with_state(recover_req(2, false, true)).success());
    REQUIRE(helper.recover_with_state(recover_req(-1, true, std::nullopt)).success());
    CHECK(helper.captured_gcodes ==
          std::vector<std::string>{"MMU_RECOVER GATE=2 LOADED=1", "MMU_RECOVER BYPASS=1"});
}

TEST_CASE("Happy Hare recover_with_state refuses what the MMU cannot be in",
          "[ams][happy_hare][recovery]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    SECTION("not running") {
        CHECK_FALSE(helper.recover_with_state(recover_req(0, false, true)).success());
    }
    SECTION("gate out of range") {
        helper.set_running(true);
        CHECK(helper.recover_with_state(recover_req(4, false, std::nullopt)).result ==
              AmsResult::INVALID_SLOT);
    }
    SECTION("bypass on an MMU without one") {
        helper.set_running(true);
        helper.set_supports_bypass(false);
        CHECK_FALSE(helper.recover_with_state(recover_req(-1, true, std::nullopt)).success());
    }
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("Happy Hare error-popup Recover lets HH detect the filament position",
          "[ams][happy_hare][recovery]") {
    // Our loaded flag reads false for every position HH calls Unknown, so
    // echoing it as LOADED=0 would tell HH "unloaded" about filament stuck
    // mid-bowden. Bare MMU_RECOVER has HH read its own sensors.
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    hh.initialize_test_gates(4);
    struct Pos {
        int filament_pos;
        const char* filament;
    };
    const Pos pos = GENERATE(Pos{8, "Loaded"}, Pos{0, "Unloaded"}, Pos{3, "Unknown"});
    nlohmann::json mmu;
    mmu["action"] = "Idle";
    mmu["print_state"] = "pause_locked";
    mmu["filament_pos"] = pos.filament_pos;
    mmu["filament"] = pos.filament;
    mmu["reason_for_pause"] = "Clog detected";
    hh.test_parse_mmu_state(mmu);

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto ev = hh.classify_error("!! Clog detected", ctx);
    REQUIRE(ev.has_value());
    std::string recover_gcode;
    for (const auto& a : ev->recovery_actions)
        if (a.log_tag == "hh::recover")
            recover_gcode = a.gcode;
    INFO(pos.filament);
    CHECK(recover_gcode == "MMU_RECOVER");
}

// ============================================================================
// preload_lane() Tests
// ============================================================================

TEST_CASE("Happy Hare preload_lane sends MMU_PRELOAD for the gate", "[ams][happy_hare][preload]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.set_running(true);

    CHECK(helper.supports_lane_preload());
    REQUIRE(helper.preload_lane(2).success());
    CHECK(helper.captured_gcodes == std::vector<std::string>{"MMU_PRELOAD GATE=2"});

    helper.clear_captured_gcodes();
    CHECK(helper.preload_lane(4).result == AmsResult::INVALID_SLOT);
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("Happy Hare preload_lane is refused while printing", "[ams][happy_hare][preload]") {
    // Happy Hare's MMU_PRELOAD opens with check_if_printing().
    PrintLifecycleFixture fx;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(fx.api.get(),
                                                                             &fx.mock_client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.set_running(true);
    helix::test::set_wire_state(fx.state, helix::PrintJobState::PRINTING);

    CHECK(helper.preload_lane(1).result == AmsResult::WRONG_STATE);
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("Happy Hare advertises lane eject, gate select and gate check; Reset reads Home",
          "[ams][happy_hare][capability]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    CHECK(helper.supports_lane_eject());
    CHECK(helper.supports_gate_select());
    CHECK(helper.supports_gate_check());
    CHECK(helper.reset_button_label() == "Home");
}

TEST_CASE("path_segment_from_happy_hare_pos maps every filament_pos and rejects the rest",
          "[ams][happy_hare][v4][edge]") {
    const std::pair<int, PathSegment> rows[] = {
        {-100, PathSegment::NONE}, {-1, PathSegment::NONE},  {0, PathSegment::SPOOL},
        {1, PathSegment::PREP},    {2, PathSegment::PREP},   {3, PathSegment::LANE},
        {4, PathSegment::HUB},     {5, PathSegment::OUTPUT}, {6, PathSegment::TOOLHEAD},
        {7, PathSegment::NOZZLE},  {8, PathSegment::NOZZLE}, {9, PathSegment::NOZZLE},
        {10, PathSegment::NOZZLE}, {11, PathSegment::NONE},  {255, PathSegment::NONE},
    };
    for (const auto& [pos, segment] : rows) {
        INFO("filament_pos " << pos);
        CHECK(path_segment_from_happy_hare_pos(pos) == segment);
    }
}

TEST_CASE("ams_action_from_string maps every Happy Hare action, unknown to IDLE",
          "[ams][happy_hare][v4][edge]") {
    const std::pair<const char*, AmsAction> rows[] = {
        {"Idle", AmsAction::IDLE},
        {"Loading", AmsAction::LOADING},
        {"Loading Ext", AmsAction::LOADING},
        {"Unloading", AmsAction::UNLOADING},
        {"Exiting Ext", AmsAction::UNLOADING},
        {"Selecting", AmsAction::SELECTING},
        {"Homing", AmsAction::RESETTING},
        {"Resetting", AmsAction::RESETTING},
        {"Cutting", AmsAction::CUTTING},
        {"Cutting Tip", AmsAction::CUTTING},
        {"Cutting Filament", AmsAction::CUTTING},
        {"Forming Tip", AmsAction::FORMING_TIP},
        {"Heating", AmsAction::HEATING},
        {"Checking", AmsAction::CHECKING},
        {"Purging", AmsAction::PURGING},
        {"Paused (user)", AmsAction::PAUSED},
        {"Error: filament jam", AmsAction::ERROR},
        {"SomeNewV5Action", AmsAction::IDLE},
        {"", AmsAction::IDLE},
    };
    for (const auto& [text, action] : rows) {
        INFO("action \"" << text << "\"");
        CHECK(ams_action_from_string(text) == action);
    }
}

TEST_CASE("Happy Hare per-gate arrays land on their own gates and skip unreadable entries",
          "[ams][happy_hare][v4][edge]") {
    // A short array leaves the remaining gates alone; a wrong-typed entry
    // leaves its gate alone; a negative spool id is Happy Hare's "unlinked".
    enum class Field { Temp, Name, Spool };
    struct Row {
        const char* name;
        Field field;
        int gates;
        std::vector<nlohmann::json> frames;
        std::vector<std::string> expect;
    };
    using J = nlohmann::json;
    const Row rows[] = {
        {"temperatures",
         Field::Temp,
         4,
         {J{{"gate_temperature", {210, 220, 230, 240}}}},
         {"210", "220", "230", "240"}},
        {"temperatures of the wrong type",
         Field::Temp,
         4,
         {J{{"gate_temperature", {210, "not_a_number", 230, nullptr}}}},
         {"210", "0", "230", "0"}},
        {"fewer temperatures than gates",
         Field::Temp,
         8,
         {J{{"gate_temperature", {200, 210, 220, 230}}}},
         {"200", "210", "220", "230", "0", "0", "0", "0"}},
        {"names",
         Field::Name,
         4,
         {J{{"gate_name", {"Red PLA", "Blue PETG", "Black ABS", ""}}}},
         {"Red PLA", "Blue PETG", "Black ABS", ""}},
        {"all names empty", Field::Name, 4, {J{{"gate_name", {"", "", "", ""}}}}, {"", "", "", ""}},
        {"spool ids",
         Field::Spool,
         4,
         {J{{"gate_spool_id", {5, 0, 12, 99}}}},
         {"5", "0", "12", "99"}},
        {"spool ids of the wrong type",
         Field::Spool,
         3,
         {J{{"gate_spool_id", {"bad", 7, nullptr}}}},
         {"0", "7", "0"}},
        {"a negative spool id unlinks",
         Field::Spool,
         2,
         {J{{"gate_spool_id", {10, 20}}}, J{{"gate_spool_id", {-1, 20}}}},
         {"0", "20"}},
        {"fewer spool ids than gates",
         Field::Spool,
         4,
         {J{{"gate_spool_id", {3, 8}}}},
         {"3", "8", "0", "0"}},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(row.gates);
        for (const auto& frame : row.frames) {
            helper.test_parse_mmu_state(frame);
        }
        REQUIRE(helper.get_system_info().total_slots == row.gates);
        for (int g = 0; g < row.gates; ++g) {
            INFO("gate " << g);
            const SlotInfo slot = helper.get_slot_info(g);
            switch (row.field) {
            case Field::Temp:
                CHECK(std::to_string(slot.nozzle_temp_min) == row.expect[g]);
                CHECK(slot.nozzle_temp_max == slot.nozzle_temp_min);
                break;
            case Field::Name:
                CHECK(slot.color_name == row.expect[g]);
                break;
            case Field::Spool:
                CHECK(std::to_string(slot.spoolman_id) == row.expect[g]);
                break;
            }
        }
    }
}

TEST_CASE("Happy Hare bowden_progress is clamped to -1..100 and ignores non-integers",
          "[ams][happy_hare][v4][bowden][edge]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    CHECK(helper.get_bowden_progress() == -1); // not available until reported

    // Applied in order: a string leaves the previous value standing.
    const std::pair<nlohmann::json, int> steps[] = {
        {75, 75}, {-1, -1}, {0, 0}, {100, 100}, {50, 50}, {"invalid", 50}, {150, 100}, {-5, -1},
    };
    for (const auto& [value, expected] : steps) {
        INFO("bowden_progress " << value.dump());
        helper.test_parse_mmu_state({{"bowden_progress", value}});
        CHECK(helper.get_bowden_progress() == expected);
    }
}

TEST_CASE("SpoolmanMode reads Happy Hare's spellings and defaults to OFF",
          "[ams][happy_hare][v4][edge]") {
    const std::pair<const char*, SpoolmanMode> rows[] = {
        {"off", SpoolmanMode::OFF},           {"Off", SpoolmanMode::OFF},
        {"readonly", SpoolmanMode::READONLY}, {"Read Only", SpoolmanMode::READONLY},
        {"push", SpoolmanMode::PUSH},         {"Push", SpoolmanMode::PUSH},
        {"pull", SpoolmanMode::PULL},         {"Pull", SpoolmanMode::PULL},
        {"PUSH", SpoolmanMode::OFF},          {"", SpoolmanMode::OFF},
        {"sync", SpoolmanMode::OFF},          {"unknown", SpoolmanMode::OFF},
    };
    for (const auto& [text, mode] : rows) {
        INFO("spoolman_support \"" << text << "\"");
        CHECK(spoolman_mode_from_string(text) == mode);
    }
    CHECK(std::string(spoolman_mode_to_string(SpoolmanMode::OFF)) == "Off");
    CHECK(std::string(spoolman_mode_to_string(SpoolmanMode::PUSH)) == "Push");
    CHECK(std::string(spoolman_mode_to_string(SpoolmanMode::PULL)) == "Pull");
}

TEST_CASE("Happy Hare v4 status fields: values, defaults, wrong types and deltas",
          "[ams][happy_hare][v4][status][edge]") {
    // Each field keeps its default against a frame that omits it or sends the
    // wrong type, and frames merge as deltas.
    using J = nlohmann::json;
    struct Expect {
        std::string espooler;
        std::string sync_state;
        bool sync_drive;
        int clog;
        int flow;
        float purge;
        SpoolmanMode spoolman;
        int pending;
    };
    const Expect defaults{"", "", false, 0, -1, 0.0f, SpoolmanMode::OFF, -1};
    struct Row {
        const char* name;
        std::vector<J> frames;
        Expect expect;
    };
    const Row rows[] = {
        {"every field",
         {J{{"espooler_active", "rewind"},
            {"sync_feedback_state", "tension"},
            {"sync_drive", true},
            {"clog_detection_enabled", 2},
            {"encoder", {{"flow_rate", 95}}},
            {"toolchange_purge_volume", 150.5}}},
         {"rewind", "tension", true, 2, 95, 150.5f, SpoolmanMode::OFF, -1}},
        {"a v3 frame", {J{{"gate_status", {1, 1, 1, 1}}}}, defaults},
        {"spoolman fields",
         {J{{"spoolman_support", "pull"}, {"pending_spool_id", 42}}},
         {"", "", false, 0, -1, 0.0f, SpoolmanMode::PULL, 42}},
        {"wrong types",
         {J{{"espooler_active", 42},
            {"sync_feedback_state", true},
            {"sync_drive", "yes"},
            {"clog_detection_enabled", "2"},
            {"encoder", "not_object"},
            {"toolchange_purge_volume", "big"}}},
         defaults},
        {"encoder without flow_rate", {J{{"encoder", {{"some_other_field", 42}}}}}, defaults},
        {"a float flow_rate truncates",
         {J{{"encoder", {{"flow_rate", 95.7}}}}},
         {"", "", false, 0, 95, 0.0f, SpoolmanMode::OFF, -1}},
        {"separate frames merge",
         {J{{"espooler_active", "rewind"}}, J{{"clog_detection_enabled", 1}}},
         {"rewind", "", false, 1, -1, 0.0f, SpoolmanMode::OFF, -1}},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        for (const auto& frame : row.frames) {
            helper.test_parse_mmu_state(frame);
        }
        const auto info = helper.get_system_info();
        CHECK(info.espooler_state == row.expect.espooler);
        CHECK(info.sync_feedback_state == row.expect.sync_state);
        CHECK(info.sync_drive == row.expect.sync_drive);
        CHECK(info.clog_detection == row.expect.clog);
        CHECK(info.encoder_flow_rate == row.expect.flow);
        CHECK(info.toolchange_purge_volume == Catch::Approx(row.expect.purge));
        CHECK(info.spoolman_mode == row.expect.spoolman);
        CHECK(info.pending_spool_id == row.expect.pending);
    }
}

// --- #1281 step 7: external re-bind ---

TEST_CASE("Happy Hare external re-bind clears our override (#1281 step 7)",
          "[ams][happy_hare][override-merge]") {
    // classify_binding()'s verdict matrix pins the pure function; this pins
    // the wiring on the live gate_spool_id path. Another writer (Mainsail, an
    // HH macro) re-binds gate 0 to a different spool: firmware truth must win
    // and our whole override record must drop, never setting-gated.
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);

    helix::ams::FilamentSlotOverride o;
    o.spoolman_id = 42;
    o.brand = "Polymaker";
    helper.set_gate_override(0, o);

    helper.feed_mmu_gate_spool_ids({169, 0});

    CHECK(helper.get_slot_info(0).spoolman_id == 169); // firmware truth paints
    CHECK(helper.get_slot_info(0).brand.empty());      // our stale brand no longer shadows
    CHECK_FALSE(helper.has_gate_override(0));          // record dropped
}

TEST_CASE("Happy Hare splits gates across units from per-unit counts or evenly",
          "[ams][happy_hare][v4][multi-unit][edge]") {
    // Per-unit counts (unit_gate_counts, else a num_gates string) apply only
    // when they sum to the gate count and match num_units; otherwise the
    // gates split evenly. num_units below 1 means one unit.
    using J = nlohmann::json;
    struct Row {
        const char* name;
        J setup; // sent first; null = none
        J frame;
        std::vector<int> counts;
    };
    const std::vector<int> g8(8, 1), g10(10, 1);
    const Row rows[] = {
        {"num_gates string",
         J{{"num_units", 2}},
         J{{"num_gates", "6,4"}, {"gate_status", {1, 1, 0, 1, 1, 1, 1, 0, 1, 1}}},
         {6, 4}},
        {"no per-unit counts", J{{"num_units", 2}}, J{{"gate_status", g8}}, {4, 4}},
        {"counts that do not sum to the gates",
         J{{"num_units", 2}},
         J{{"num_gates", "6,4"}, {"gate_status", g8}},
         {4, 4}},
        {"unit_gate_counts outranks num_gates",
         J{{"num_units", 2}},
         J{{"num_gates", "5,5"}, {"unit_gate_counts", {3, 7}}, {"gate_status", g10}},
         {3, 7}},
        {"an unreadable token",
         J{{"num_units", 2}},
         J{{"num_gates", "6,abc"}, {"gate_status", g8}},
         {4, 4}},
        {"one unit", J(), J{{"num_units", 1}, {"gate_status", {1, 1, 1, 1}}}, {4}},
        {"num_units 0", J(), J{{"num_units", 0}, {"gate_status", {1, 1, 1, 1}}}, {4}},
        {"num_units -1", J(), J{{"num_units", -1}, {"gate_status", {1, 1, 1, 1}}}, {4}},
        {"with an active unit",
         J(),
         J{{"num_units", 2}, {"unit", 1}, {"gate_status", {1, 1, 1, 1, 0, 0, 0, 0}}},
         {4, 4}},
        {"num_gates as a one-entry array", J(), J{{"gate_status", g8}, {"num_gates", {8}}}, {8}},
        {"num_gates as a per-unit array",
         J{{"num_units", 2}},
         J{{"gate_status", g10}, {"num_gates", {6, 4}}},
         {6, 4}},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        if (!row.setup.is_null())
            helper.test_parse_mmu_state(row.setup);
        helper.test_parse_mmu_state(row.frame);

        const auto info = helper.get_system_info();
        REQUIRE(info.units.size() == row.counts.size());
        int first = 0;
        for (size_t u = 0; u < row.counts.size(); ++u) {
            INFO("unit " << u);
            CHECK(info.units[u].slot_count == row.counts[u]);
            CHECK(info.units[u].first_slot_global_index == first);
            first += row.counts[u];
        }
        CHECK(info.total_slots == first);
        if (row.counts.size() == 1)
            CHECK(info.units[0].name == "MMU");
    }
}

TEST_CASE("Happy Hare translates espooler and sync feedback into capability fields",
          "[ams][happy_hare][v4][buffer]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    struct Case {
        const char* espooler;
        SpoolMotorAction action;
        const char* sync;
        std::optional<BufferMotion> motion;
    };
    const Case cases[] = {
        {"rewind", SpoolMotorAction::Rewinding, "compressed", BufferMotion::Advancing},
        {"assist", SpoolMotorAction::Assisting, "tension", BufferMotion::Trailing},
        {"wiggle", SpoolMotorAction::Other, "neutral", BufferMotion::Unknown},
        {"", SpoolMotorAction::None, "disabled", std::nullopt},
    };
    for (const auto& c : cases) {
        CAPTURE(c.espooler, c.sync);
        helper.test_parse_mmu_state(
            {{"espooler_active", c.espooler}, {"sync_feedback_state", c.sync}});
        const auto info = helper.get_system_info();
        CHECK(info.spool_motor == c.action);
        CHECK(info.system_buffer_motion == c.motion);
    }
}

TEST_CASE("Happy Hare device sections, in order", "[ams][happy_hare][device_actions]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto sections = helper.get_device_sections();

    REQUIRE(sections.size() == 5);
    CHECK(sections[0].id == "setup");
    CHECK(sections[1].id == "speed");
    CHECK(sections[2].id == "toolhead");
    CHECK(sections[2].label == "Toolhead");
    CHECK(sections[3].id == "accessories");
    CHECK(sections[4].id == "maintenance");
}

TEST_CASE("execute_device_action sends each setting's command and keeps the value",
          "[ams][happy_hare][device_actions][v4][edge]") {
    // `sent` is a fragment of the command (an exact command when `exact`); ""
    // means the action is refused. `kept` is what the settings list shows after.
    using Value = std::variant<std::monostate, double, bool>;
    struct Row {
        const char* id;
        std::any value;
        const char* sent;
        bool exact;
        Value kept;
    };
    const Row rows[] = {
        {"gear_from_buffer_speed", 200.0, "GEAR_FROM_BUFFER_SPEED=200", false, 200.0},
        {"gear_from_spool_speed", 80.0, "GEAR_FROM_SPOOL_SPEED=80", false, {}},
        {"gear_unload_speed", 90.0, "GEAR_UNLOAD_SPEED=90", false, {}},
        {"selector_speed", 200.0, "SELECTOR_MOVE_SPEED=200", false, {}},
        {"extruder_load_speed", 45.0, "EXTRUDER_LOAD_SPEED=45", false, {}},
        {"extruder_unload_speed", 50.0, "EXTRUDER_UNLOAD_SPEED=50", false, {}},
        {"toolhead_sensor_to_nozzle", 58.5, "TOOLHEAD_SENSOR_TO_NOZZLE=58.5", false, 58.5},
        {"toolhead_extruder_to_nozzle", 72.3, "TOOLHEAD_EXTRUDER_TO_NOZZLE=72.3", false, {}},
        {"toolhead_entry_to_extruder", 0.0, "TOOLHEAD_ENTRY_TO_EXTRUDER=0.0", false, {}},
        {"toolhead_ooze_reduction", 2.5, "TOOLHEAD_OOZE_REDUCTION=2.5", false, {}},
        {"sync_to_extruder", true, "SYNC_TO_EXTRUDER=1", false, true},
        {"sync_to_extruder", false, "SYNC_TO_EXTRUDER=0", false, {}},
        {"clog_detection",
         std::string("Auto"),
         "MMU_TEST_CONFIG ENABLE_CLOG_DETECTION=2",
         true,
         {}},
        {"clog_detection",
         std::string("Manual"),
         "MMU_TEST_CONFIG ENABLE_CLOG_DETECTION=1",
         true,
         {}},
        {"clog_detection", std::string("Off"), "MMU_TEST_CONFIG ENABLE_CLOG_DETECTION=0", true, {}},
        {"espooler_mode", std::string("rewind"), "MMU_ESPOOLER OPERATION=rewind", true, {}},
        {"espooler_mode", std::any(), "", false, {}}, // no operation named
        {"test_move", std::any(), "MMU_TEST_MOVE", true, {}},
        {"servo_up", std::any(), "MMU_SERVO POS=up", true, {}},
        {"servo_move", std::any(), "MMU_SERVO POS=move", true, {}},
        {"servo_down", std::any(), "MMU_SERVO POS=down", true, {}},
        {"gear_sync", true, "MMU_SYNC_GEAR_MOTOR SYNC=1", true, {}},
        {"gear_sync", false, "MMU_SYNC_GEAR_MOTOR SYNC=0", true, {}},
        {"servo_buzz", std::any(), "MMU_SERVO", true, {}}, // no BUZZ=1 argument
        {"calibrate_servo", std::any(), "", false, {}},    // not an action
    };
    for (const auto& row : rows) {
        INFO(row.id << (row.value.has_value() ? "" : " (no value)") << " -> " << row.sent);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.set_config_defaults_for_test(); // config-backed actions are offered once it loads
        helper.initialize_test_gates(4);
        helper.set_running(true);

        const AmsError err = helper.execute_device_action(row.id, row.value);
        if (!*row.sent) {
            CHECK_FALSE(err.success());
            continue;
        }
        CHECK(err.success());
        CHECK((row.exact ? helper.has_gcode(row.sent) : helper.has_gcode_containing(row.sent)));
        CHECK_FALSE(helper.has_gcode_containing("BUZZ=1"));
        if (std::holds_alternative<std::monostate>(row.kept))
            continue;
        const auto actions = helper.get_device_actions();
        const auto it = std::find_if(actions.begin(), actions.end(),
                                     [&](const auto& a) { return a.id == row.id; });
        REQUIRE(it != actions.end());
        if (const auto* d = std::get_if<double>(&row.kept))
            CHECK(std::any_cast<double>(it->current_value) == Catch::Approx(*d));
        else
            CHECK(std::any_cast<bool>(it->current_value) == std::get<bool>(row.kept));
    }
}

TEST_CASE("Happy Hare without drying_state has no dryer and refuses start and stop",
          "[ams][happy_hare][v4][edge]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    CHECK_FALSE(helper.get_dryer_info().supported);
    const AmsError start = helper.start_drying(55.0f, 240);
    CHECK(start.result == AmsResult::NOT_SUPPORTED);
    const AmsError stop = helper.stop_drying();
    CHECK(stop.result == AmsResult::NOT_SUPPORTED);
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("Happy Hare reads an object drying_state, defaulting fields it omits",
          "[ams][happy_hare][v4][edge]") {
    struct Row {
        const char* name;
        nlohmann::json state;
        bool active;
        float current;
        float target;
        int remaining;
        int duration;
        int fan;
    };
    const Row rows[] = {
        {"every field",
         {{"active", true},
          {"current_temp", 52.3},
          {"target_temp", 55.0},
          {"remaining_min", 120},
          {"duration_min", 240},
          {"fan_pct", 50}},
         true,
         52.3f,
         55.0f,
         120,
         240,
         50},
        {"some fields", {{"active", false}, {"current_temp", 25.0}}, false, 25.0f, 0.0f, 0, -1, -1},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.test_parse_mmu_state({{"drying_state", row.state}});

        const auto dryer = helper.get_dryer_info();
        CHECK(dryer.supported);
        CHECK(dryer.active == row.active);
        CHECK(dryer.current_temp_c == Catch::Approx(row.current));
        CHECK(dryer.target_temp_c == Catch::Approx(row.target));
        CHECK(dryer.remaining_min == row.remaining);
        if (row.duration >= 0)
            CHECK(dryer.duration_min == row.duration);
        if (row.fan >= 0)
            CHECK(dryer.fan_pct == row.fan);
    }
}

TEST_CASE("Happy Hare drying sends MMU_HEATER with TIMER minutes, per unit on multi-unit",
          "[ams][happy_hare][v4][edge]") {
    // TIMER= is minutes; MMU_HEATER takes no fan setting, so none is ever sent.
    struct Row {
        const char* name;
        std::vector<int> units; // empty = one 4-gate unit
        int unit;
        float temp;
        int minutes;
        int fan;
        const char* start;
        const char* stop;
    };
    const Row rows[] = {
        {"one unit",
         {},
         0,
         55.0f,
         240,
         50,
         "MMU_HEATER DRY=1 TEMP=55 TIMER=240",
         "MMU_HEATER STOP=1"},
        {"no fan given",
         {},
         0,
         45.0f,
         120,
         -1,
         "MMU_HEATER DRY=1 TEMP=45 TIMER=120",
         "MMU_HEATER STOP=1"},
        {"unit 1 owns gates 4-7",
         {4, 4},
         1,
         50.0f,
         120,
         -1,
         "MMU_HEATER DRY=1 TEMP=50 TIMER=120 GATES=4,5,6,7",
         "MMU_HEATER STOP=1 GATES=4,5,6,7"},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.test_set_clock([] { return std::time_t{1000}; });
        helper.test_parse_mmu_state({{"drying_state", {{"active", false}}}});
        if (row.units.empty())
            helper.initialize_test_gates(4);
        else
            helper.initialize_test_gates_multi(row.units);

        REQUIRE(helper.start_drying(row.temp, row.minutes, row.fan, row.unit).success());
        CHECK(helper.has_gcode(row.start));
        CHECK_FALSE(helper.has_gcode_containing("FAN="));
        if (row.units.empty())
            CHECK(helper.get_dryer_info().remaining_min == row.minutes);

        helper.clear_captured_gcodes();
        REQUIRE(helper.stop_drying(row.unit).success());
        CHECK(helper.has_gcode(row.stop));
    }
}

TEST_CASE("Happy Hare reads filament_heater + heater_max_temp from config",
          "[ams][happy_hare][v4]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    nlohmann::json settings = {{"mmu_machine", {{"filament_heater", "heater_generic box1_heater"}}},
                               {"mmu", {{"heater_max_temp", 65.0}}}};
    helper.test_apply_heater_config(settings);
    auto d = helper.get_dryer_info();
    REQUIRE(d.supported);
    REQUIRE(d.max_temp_c == Catch::Approx(65.0f));
}

TEST_CASE("Happy Hare unit environment: heater temperature and the sensor's humidity",
          "[ams][happy_hare][v4]") {
    // The heater is named verbatim (no forced heater_generic prefix). An
    // environment_sensor alias ("temperature_sensor box") resolves to whichever
    // humidity chip backs it, including the QIDI Box's aht20_f (#1022). A sensor
    // alone still surfaces: no heater reading has to arrive first.
    using J = nlohmann::json;
    const J box_with_sensor = {{"mmu_machine",
                                {{"filament_heater", "heater_generic box1_heater"},
                                 {"environment_sensor", "temperature_sensor box"}}},
                               {"mmu", {{"heater_max_temp", 65.0}}}};
    const J drying_at_50 = {{"active", true}, {"current_temp", 50.0}};
    struct Row {
        const char* name;
        J settings;      // null = no config query
        J drying_state;  // sent as printer.mmu.drying_state
        J heater_status; // null = none
        J sensor_status; // null = none
        float temp;
        float humidity; // < 0 = no humidity shown
    };
    const Row rows[] = {
        {"a sensor and no heater", J{{"mmu_machine", {{"environment_sensor", "bme280 mmu_box"}}}},
         J::array({"", "", "", ""}), nullptr,
         J{{"bme280 mmu_box", {{"temperature", 24.5}, {"humidity", 38.0}}}}, 24.5f, 38.0f},
        {"a heater outside heater_generic",
         J{{"mmu_machine", {{"filament_heater", "my_heater chamber"}}}}, J::array({"", "", "", ""}),
         J{{"my_heater chamber", {{"temperature", 50.0}}}}, nullptr, 50.0f, -1},
        {"drying_state's own temperature", nullptr, J{{"active", true}, {"current_temp", 48.5}},
         nullptr, nullptr, 48.5f, -1},
        {"humidity from an htu21d chip", box_with_sensor, drying_at_50, nullptr,
         J{{"htu21d box", {{"temperature", 50.0}, {"humidity", 37.5}}}}, 50.0f, 37.5f},
        {"humidity from an aht20_f chip", box_with_sensor, drying_at_50, nullptr,
         J{{"aht20_f box", {{"temperature", 27.67}, {"humidity", 16.0}}}}, -1, 16.0f},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        REQUIRE_FALSE(helper.get_system_info().units[0].environment.has_value());

        if (!row.settings.is_null())
            helper.test_apply_heater_config(row.settings);
        helper.test_parse_mmu_state({{"drying_state", row.drying_state}});
        if (!row.heater_status.is_null())
            helper.test_apply_filament_heater_status(row.heater_status);
        if (!row.sensor_status.is_null())
            CHECK(helper.test_apply_environment_sensor_status(row.sensor_status));

        const auto info = helper.get_system_info();
        REQUIRE(info.units[0].environment.has_value());
        const auto& env = *info.units[0].environment;
        if (row.temp >= 0)
            CHECK(env.temperature_c == Catch::Approx(row.temp));
        CHECK(env.has_humidity == (row.humidity >= 0));
        if (row.humidity >= 0)
            CHECK(env.humidity_pct == Catch::Approx(row.humidity));
    }

    SECTION("a frame naming no humidity chip keeps the last reading") {
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.test_apply_heater_config(box_with_sensor);
        helper.test_parse_mmu_state({{"drying_state", drying_at_50}});
        REQUIRE(helper.test_apply_environment_sensor_status(
            {{"htu21d box", {{"temperature", 50.0}, {"humidity", 37.5}}}}));
        CHECK_FALSE(helper.test_apply_environment_sensor_status(
            {{"toolhead", {{"position", {0, 0, 0, 0}}}}}));
        CHECK(helper.get_system_info().units[0].environment->humidity_pct == Catch::Approx(37.5f));
    }
}

TEST_CASE("Happy Hare per-gate sensors map to the right unit (multi-MMU)",
          "[ams][happy_hare][v4]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    // Two MMU units of 2 gates each (gates 0-1 = unit 0, gates 2-3 = unit 1).
    helper.initialize_test_units({2, 2});

    // Per-gate (EMU) config: one heater + one env sensor per gate. Comma-string
    // form (as Moonraker often returns getlist values) is parsed too.
    nlohmann::json settings = {
        {"mmu_machine",
         {{"filament_heaters", "heater_generic h0, heater_generic h1, "
                               "heater_generic h2, heater_generic h3"},
          {"environment_sensors",
           nlohmann::json::array({"temperature_sensor s0", "temperature_sensor s1",
                                  "temperature_sensor s2", "temperature_sensor s3"})}}},
        {"mmu", {{"heater_max_temp", 65.0}}}};
    helper.test_apply_heater_config(settings);

    // Live readings: unit 0's first gate (gate 0) heater h0 + sensor s0;
    // unit 1's first gate (gate 2) heater h2 + sensor s2. Distinct values.
    nlohmann::json status = {{"heater_generic h0", {{"temperature", 40.0}}},
                             {"heater_generic h2", {{"temperature", 55.0}}},
                             {"htu21d s0", {{"humidity", 20.0}}},
                             {"htu21d s2", {{"humidity", 80.0}}}};
    REQUIRE(helper.test_apply_filament_heater_status(status));
    REQUIRE(helper.test_apply_environment_sensor_status(status));

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);

    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[0].environment->temperature_c == Catch::Approx(40.0f));
    REQUIRE(info.units[0].environment->humidity_pct == Catch::Approx(20.0f));

    REQUIRE(info.units[1].environment.has_value());
    REQUIRE(info.units[1].environment->temperature_c == Catch::Approx(55.0f));
    REQUIRE(info.units[1].environment->humidity_pct == Catch::Approx(80.0f));
}

// --- Backwards compatibility: v3 sends nothing new ---

TEST_CASE("Happy Hare v3 data with no v4 fields works normally", "[ams][happy_hare][v4][compat]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;

    // Pure v3 data — only classic fields
    nlohmann::json mmu_data = {{"gate", 2},
                               {"tool", 2},
                               {"filament", "Loaded"},
                               {"action", "Idle"},
                               {"filament_pos", 8},
                               {"has_bypass", true},
                               {"gate_status", {1, 0, 2, 1}},
                               {"gate_color_rgb", {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00}},
                               {"gate_material", {"PLA", "PETG", "ABS", "TPU"}},
                               {"ttg_map", {0, 1, 2, 3}},
                               {"endless_spool_groups", {0, 0, 1, 1}}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.type == AmsType::HAPPY_HARE);
    REQUIRE(info.current_slot == 2);
    REQUIRE(info.current_tool == 2);
    REQUIRE(info.filament_loaded == true);
    REQUIRE(info.action == AmsAction::IDLE);
    REQUIRE(info.supports_bypass == true);
    REQUIRE(info.total_slots == 4);

    // All v4 fields should be at safe defaults
    REQUIRE(info.spoolman_mode == SpoolmanMode::OFF);
    REQUIRE(info.pending_spool_id == -1);
    REQUIRE(info.espooler_state.empty());
    REQUIRE(info.sync_feedback_state.empty());
    REQUIRE(info.sync_drive == false);
    REQUIRE(info.clog_detection == 0);
    REQUIRE(info.encoder_flow_rate == -1);
    REQUIRE(info.toolchange_purge_volume == 0.0f);

    // Bowden progress not available
    REQUIRE(helper.get_bowden_progress() == -1);

    // Dryer not available
    auto dryer = helper.get_dryer_info();
    REQUIRE_FALSE(dryer.supported);

    // Slot data should be properly parsed
    auto slot0 = helper.get_slot_info(0);
    REQUIRE(slot0.color_rgb == 0xFF0000);
    REQUIRE(slot0.material == "PLA");
    REQUIRE(slot0.status == SlotStatus::AVAILABLE);

    auto slot2 = helper.get_slot_info(2);
    REQUIRE(slot2.color_rgb == 0x0000FF);
    REQUIRE(slot2.material == "ABS");
    // gate_status=2 maps to FROM_BUFFER, but gate 2 is also the gate Happy Hare
    // reports loaded, and the loaded gate outranks its fill state (#1199). An
    // unloaded from_buffer gate still reads FROM_BUFFER (case below).
    REQUIRE(slot2.status == SlotStatus::LOADED);
}

// --- has_bypass: the single flag behind the whole bypass UI ---

// printer.mmu.has_bypass gates the sidebar toggle, the Device Operations bypass
// section and the bypass node on the filament path. Happy Hare defaults it to 0
// for mmu_vendor "Other" (which is what a Qidi Box under HH reports) and on
// type-A selectors ANDs it with the calibrated bypass offset, so it is both
// legitimately false on real hardware and able to turn true later.
TEST_CASE_METHOD(HelixTestFixture, "Happy Hare has_bypass drives supports_bypass",
                 "[ams][happy_hare][bypass]") {
    auto base_mmu = []() {
        return nlohmann::json{{"gate", 0},
                              {"tool", 0},
                              {"filament", "Loaded"},
                              {"action", "Idle"},
                              {"filament_pos", 8},
                              {"gate_status", {1, 0, 2, 1}},
                              {"gate_material", {"PLA", "PETG", "ABS", "TPU"}},
                              {"ttg_map", {0, 1, 2, 3}}};
    };

    SECTION("stays off until the firmware confirms, so the UI never flickers") {
        AmsBackendHappyHareTestHelper helper;
        REQUIRE(helper.get_system_info().supports_bypass == false);

        nlohmann::json mmu = base_mmu();
        mmu["has_bypass"] = false;
        helper.test_parse_mmu_state(mmu);

        REQUIRE(helper.get_system_info().supports_bypass == false);
    }

    SECTION("firmware true turns it on") {
        AmsBackendHappyHareTestHelper helper;

        nlohmann::json mmu = base_mmu();
        mmu["has_bypass"] = true;
        helper.test_parse_mmu_state(mmu);

        REQUIRE(helper.get_system_info().supports_bypass == true);
    }

    SECTION("absent field assumes supported rather than removing the control") {
        AmsBackendHappyHareTestHelper helper;
        helper.test_parse_mmu_state(base_mmu());

        REQUIRE(helper.get_system_info().supports_bypass == true);
    }

    SECTION("turns back on when the selector is calibrated mid-session") {
        AmsBackendHappyHareTestHelper helper;

        nlohmann::json mmu = base_mmu();
        mmu["has_bypass"] = false;
        helper.test_parse_mmu_state(mmu);
        REQUIRE(helper.get_system_info().supports_bypass == false);

        mmu["has_bypass"] = true;
        helper.test_parse_mmu_state(mmu);
        REQUIRE(helper.get_system_info().supports_bypass == true);
    }

    SECTION("a non-boolean value is ignored rather than coerced") {
        AmsBackendHappyHareTestHelper helper;

        nlohmann::json mmu = base_mmu();
        mmu["has_bypass"] = nullptr;
        helper.test_parse_mmu_state(mmu);

        REQUIRE(helper.get_system_info().supports_bypass == true);
    }
}

// --- v3+v4 mixed: some v4 fields with v3 base ---

TEST_CASE("Happy Hare mixed v3/v4 data parses both correctly", "[ams][happy_hare][v4][compat]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;

    nlohmann::json mmu_data = {// v3 fields
                               {"gate", 0},
                               {"tool", 0},
                               {"filament", "Loaded"},
                               {"action", "Idle"},
                               {"filament_pos", 8},
                               {"gate_status", {2, 1, 0, 1}},
                               {"gate_material", {"PLA", "PETG", "", "ABS"}},
                               // v4 fields mixed in
                               {"bowden_progress", 100},
                               {"spoolman_support", "push"},
                               {"gate_name", {"Red Spool", "", "Empty", "Black"}},
                               {"gate_temperature", {210, 230, 0, 250}},
                               {"espooler_active", "assist"},
                               {"clog_detection_enabled", 2}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    // v3 data
    REQUIRE(info.current_slot == 0);
    REQUIRE(info.filament_loaded == true);
    REQUIRE(info.total_slots == 4);

    // v4 additions
    REQUIRE(helper.get_bowden_progress() == 100);
    REQUIRE(info.spoolman_mode == SpoolmanMode::PUSH);
    REQUIRE(info.espooler_state == "assist");
    REQUIRE(info.clog_detection == 2);

    // Per-slot v4 data
    auto slot0 = helper.get_slot_info(0);
    REQUIRE(slot0.color_name == "Red Spool");
    REQUIRE(slot0.nozzle_temp_min == 210);
    REQUIRE(slot0.material == "PLA");

    auto slot3 = helper.get_slot_info(3);
    REQUIRE(slot3.color_name == "Black");
    REQUIRE(slot3.nozzle_temp_min == 250);
    REQUIRE(slot3.material == "ABS");
}

TEST_CASE("Happy Hare activates a spool only on a gate load, not on a link",
          "[ams][happy_hare][spoolman][plugin_filament]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.test_parse_mmu_state({{"spoolman_support", "push"}});
    REQUIRE(helper.manages_active_spool());
    CHECK_FALSE(helper.sets_active_spool_on_link());
}

TEST_CASE("Happy Hare reports the manages_active_spool the shared case table claims",
          "[ams][happy_hare][spoolman][plugin_filament]") {
    const auto rows = helix::test::ownership_rows_for("Happy Hare");
    REQUIRE(rows.size() >= 3);
    for (const auto& row : rows) {
        INFO(row["name"].get<std::string>());
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.test_parse_mmu_state({{"spoolman_support", row["spoolman_mode"]}});
        CHECK(helper.manages_active_spool() == row["manages_active_spool"].get<bool>());
    }

    // Beyond the shared table: OFF before anything is reported, pull mode, and
    // a mode switched back off.
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    CHECK_FALSE(helper.manages_active_spool());
    helper.test_parse_mmu_state({{"spoolman_support", "pull"}});
    CHECK(helper.manages_active_spool());
    helper.test_parse_mmu_state({{"spoolman_support", "off"}});
    CHECK_FALSE(helper.manages_active_spool());
}

// ============================================================================
// Live heater temp/target from Moonraker status
// ============================================================================

TEST_CASE("Happy Hare parses live filament_heater temp/target", "[ams][happy_hare][v4]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.test_apply_heater_config(
        nlohmann::json{{"mmu_machine", {{"filament_heater", "heater_generic box1_heater"}}},
                       {"mmu", {{"heater_max_temp", 65.0}}}});
    nlohmann::json params = {
        {"heater_generic box1_heater", {{"temperature", 48.5}, {"target", 55.0}}}};
    helper.test_apply_filament_heater_status(params);
    auto d = helper.get_dryer_info();
    REQUIRE(d.current_temp_c == Catch::Approx(48.5f).epsilon(0.01));
    REQUIRE(d.target_temp_c == Catch::Approx(55.0f).epsilon(0.01));
}

TEST_CASE("Happy Hare with a heater reads drying_state as an array or an object",
          "[ams][happy_hare][emu]") {
    // A drying_state array ships on every Happy Hare install, so a heater has to be
    // configured for the dryer to be supported at all; "complete" is not drying.
    struct Row {
        const char* name;
        nlohmann::json state;
        bool active;
    };
    const Row rows[] = {
        {"all empty", {"", "", "", ""}, false},
        {"one gate complete", {"complete", "", "", ""}, false},
        {"one gate active", {"", "active", "", ""}, true},
        {"object format", {{"active", true}, {"current_temp", 55.0}}, true},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.test_apply_heater_config(
            {{"mmu_machine", {{"filament_heater", "heater_generic MMU_heater"}}}});
        helper.test_parse_mmu_state({{"drying_state", row.state}});
        CHECK(helper.get_dryer_info().supported);
        CHECK(helper.get_dryer_info().active == row.active);
    }
}

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "gate_color hex string fallback",
                 "[ams][happy_hare][emu]") {
    initialize_test_gates(3);
    // gate_color_rgb absent, fall back to gate_color hex strings
    nlohmann::json mmu_data = {{"gate_color", {"ffffff", "000000", "042f56"}}};
    test_parse_mmu_state(mmu_data);

    auto info = get_system_info();
    REQUIRE(info.units[0].slots[0].color_rgb == 0xFFFFFF);
    REQUIRE(info.units[0].slots[1].color_rgb == 0x000000);
    REQUIRE(info.units[0].slots[2].color_rgb == 0x042F56);
}

// ============================================================================
// EMU aggregate sensor format
// ============================================================================

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "EMU aggregate sensor format",
                 "[ams][happy_hare][emu]") {
    initialize_test_gates(4);

    SECTION("aggregate sensors with active gate") {
        nlohmann::json mmu_data = {
            {"gate", 0},
            {"sensors",
             {{"mmu_pre_gate", true}, {"mmu_gear", true}, {"extruder", true}, {"toolhead", true}}}};
        test_parse_mmu_state(mmu_data);

        auto info = get_system_info();
        REQUIRE(info.units[0].has_slot_sensors == true);

        // All gates should report having pre-gate sensors
        auto gate0 = get_gate_sensor(0);
        REQUIRE(gate0 != nullptr);
        REQUIRE(gate0->has_pre_gate_sensor == true);
        REQUIRE(gate0->pre_gate_triggered == true);

        // Other gates have sensor hardware but we only know current gate's reading
        auto gate1 = get_gate_sensor(1);
        REQUIRE(gate1 != nullptr);
        REQUIRE(gate1->has_pre_gate_sensor == true);
    }

    SECTION("aggregate sensors with different active gate") {
        nlohmann::json mmu_data = {{"gate", 2},
                                   {"sensors", {{"mmu_pre_gate", false}, {"mmu_gear", true}}}};
        test_parse_mmu_state(mmu_data);

        auto gate2 = get_gate_sensor(2);
        REQUIRE(gate2 != nullptr);
        REQUIRE(gate2->has_pre_gate_sensor == true);
        REQUIRE(gate2->pre_gate_triggered == false);
    }
}

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper,
                 "Happy Hare units report the toolhead sensor the sensors map says is fitted",
                 "[ams][happy_hare][toolhead_sensor]") {
    initialize_test_gates(4);

    SECTION("fitted") {
        test_parse_mmu_state({{"sensors", {{"mmu_pre_gate_0", true}, {"toolhead", false}}}});
        CHECK(get_system_info().units[0].has_toolhead_sensor);
    }
    SECTION("absent") {
        test_parse_mmu_state({{"sensors", {{"mmu_pre_gate_0", true}}}});
        CHECK_FALSE(get_system_info().units[0].has_toolhead_sensor);
    }
    SECTION("disabled (null) on v4") {
        test_parse_mmu_state({{"sensors", {{"mmu_pre_gate_0", true}, {"toolhead", nullptr}}}});
        CHECK_FALSE(get_system_info().units[0].has_toolhead_sensor);
    }
}

// ============================================================================
// EMU gate_filament_name parsing — EMU sends filament names via
// gate_filament_name instead of gate_name
// ============================================================================

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "EMU gate_filament_name parsing",
                 "[ams][happy_hare][emu]") {
    initialize_test_gates(3);

    SECTION("gate_name takes priority over gate_filament_name") {
        nlohmann::json mmu_data = {{"gate_name", {"Priority Name", "Other", "Third"}},
                                   {"gate_filament_name", {"Fallback", "Fallback", "Fallback"}}};
        test_parse_mmu_state(mmu_data);

        auto info = get_system_info();
        REQUIRE(info.units[0].slots[0].color_name == "Priority Name");
    }

    SECTION("gate_filament_name used when gate_name absent") {
        nlohmann::json mmu_data = {{"gate_filament_name", {"Name A", "Name B", "Name C"}}};
        test_parse_mmu_state(mmu_data);

        auto info = get_system_info();
        REQUIRE(info.units[0].slots[0].color_name == "Name A");
    }
}

// ============================================================================
// Full EMU integration test — validates all EMU parsing fixes together
// using real data from an EMU user's Moonraker dump
// ============================================================================

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "Full EMU status integration",
                 "[ams][happy_hare][emu]") {
    // Real data from EMU user's Moonraker dump (simplified)
    nlohmann::json mmu_data = {
        {"gate", 0},
        {"tool", 0},
        {"filament", "Loaded"},
        {"action", "Idle"},
        {"num_gates", 8},
        {"filament_pos", 10},
        {"has_bypass", true},
        {"gate_status", {2, 2, 2, 2, 2, 2, 1, 1}},
        {"gate_color_rgb",
         {{1.0, 1.0, 1.0},
          {0.0, 0.0, 0.0},
          {0.976, 0.976, 0.4},
          {0.016, 0.184, 0.337},
          {0.553, 0.784, 0.588},
          {0.0, 0.0, 0.0},
          {1.0, 1.0, 1.0},
          {0.0, 0.0, 0.0}}},
        {"gate_material", {"PLA", "PLA", "PLA", "PLA", "PLA", "ABS", "ABS", "ASA CF"}},
        {"gate_filament_name",
         {"Matte White", "Matte Black", "Matte Yellow", "Matte Navy", "Matte Green", "Black",
          "White", "Black"}},
        {"gate_temperature", {230, 230, 230, 230, 230, 260, 260, 265}},
        {"gate_name", nullptr},
        {"ttg_map", {0, 1, 2, 3, 4, 5, 6, 7}},
        {"endless_spool_groups", {0, 1, 2, 3, 4, 5, 6, 7}},
        {"bowden_progress", -1},
        {"encoder", nullptr},
        {"unit_gate_counts", nullptr},
        {"sync_drive", true},
        {"sync_feedback_state", "neutral"},
        {"clog_detection_enabled", 0},
        {"espooler_active", ""},
        {"spoolman_support", "push"},
        {"drying_state", {"", "", "", "", "", "", "", ""}},
        {"sensors",
         {{"mmu_pre_gate", true},
          {"mmu_gear", true},
          {"filament_proportional", false},
          {"extruder", true},
          {"toolhead", true}}}};
    test_parse_mmu_state(mmu_data);

    auto info = get_system_info();

    // Structure
    REQUIRE(info.total_slots == 8);
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slot_count == 8);

    // Current state
    REQUIRE(info.current_slot == 0);
    REQUIRE(info.current_tool == 0);
    REQUIRE(info.filament_loaded == true);
    REQUIRE(info.action == AmsAction::IDLE);
    REQUIRE(info.supports_bypass == true);

    // Colors (float arrays converted to 0xRRGGBB, each channel rounded)
    REQUIRE(info.units[0].slots[0].color_rgb == 0xFFFFFF); // White
    REQUIRE(info.units[0].slots[1].color_rgb == 0x000000); // Black
    REQUIRE(info.units[0].slots[2].color_rgb == 0xF9F966); // 0.976 -> 249, 0.4 -> 102
    REQUIRE(info.units[0].slots[3].color_rgb == 0x042F56); // Navy

    // Materials
    REQUIRE(info.units[0].slots[0].material == "PLA");
    REQUIRE(info.units[0].slots[5].material == "ABS");
    REQUIRE(info.units[0].slots[7].material == "ASA CF");

    // Filament names (from gate_filament_name since gate_name is null)
    REQUIRE(info.units[0].slots[0].color_name == "Matte White");
    REQUIRE(info.units[0].slots[3].color_name == "Matte Navy");

    // Temperatures
    REQUIRE(info.units[0].slots[0].nozzle_temp_min == 230);
    REQUIRE(info.units[0].slots[5].nozzle_temp_min == 260);

    // Sensors (aggregate format)
    REQUIRE(info.units[0].has_slot_sensors == true);

    // Dryer: a stock EMU is passive, a sealed box and a sensor per lane with no heater.
    // The capture carries a drying_state array because Happy Hare's environment manager
    // is always loaded, which says nothing about whether a heater is fitted. Offering
    // drying controls here would send MMU_HEATER at a box that has none.
    auto dryer = get_dryer_info();
    REQUIRE(dryer.supported == false);
    REQUIRE(dryer.active == false);

    // v4 fields
    REQUIRE(info.sync_drive == true);
    REQUIRE(info.sync_feedback_state == "neutral");
    REQUIRE(info.spoolman_mode == SpoolmanMode::PUSH);
    REQUIRE(info.encoder_flow_rate == -1); // null encoder
}

TEST_CASE("EncoderClogInfo: clog percentage is the detection length used, clamped", "[ams][clog]") {
    struct PctRow {
        float detection_length;
        float headroom;
        int pct;
    };
    const PctRow pct_rows[] = {
        {0.0f, 5.0f, 0},     // no detection length: nothing to measure
        {12.4f, 12.4f, 0},   // full headroom
        {12.4f, 0.0f, 100},  // no headroom
        {10.0f, 3.0f, 70},   // 7mm of 10 used
        {10.0f, -1.0f, 100}, // negative headroom clamps
    };
    for (const auto& r : pct_rows) {
        INFO("length " << r.detection_length << " headroom " << r.headroom);
        EncoderClogInfo info;
        info.detection_length = r.detection_length;
        info.headroom = r.headroom;
        CHECK(info.get_clog_pct() == r.pct);
    }

    struct WarnRow {
        float desired;
        float min;
        bool warning;
    };
    const WarnRow warn_rows[] = {{5.0f, 3.0f, true}, {5.0f, 6.0f, false}, {0.0f, 0.0f, false}};
    for (const auto& r : warn_rows) {
        INFO("desired " << r.desired << " min " << r.min);
        EncoderClogInfo info;
        info.desired_headroom = r.desired;
        info.min_headroom = r.min;
        CHECK(info.is_warning() == r.warning);
    }
}

// ============================================================================
// Encoder + Flowguard JSON Parsing Tests
// ============================================================================

TEST_CASE("Happy Hare: parse full encoder object into encoder_info", "[ams][happy_hare][clog]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data;
    mmu_data["clog_detection_enabled"] = 2;
    mmu_data["encoder"] = {{"flow_rate", 85},
                           {"desired_headroom", 5.0},
                           {"detection_length", 12.4},
                           {"headroom", 8.0},
                           {"min_headroom", 4.2}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.encoder_info.enabled == true);
    REQUIRE(info.encoder_info.flow_rate == 85);
    REQUIRE(info.encoder_info.detection_mode == 2);
    REQUIRE(info.encoder_info.desired_headroom == Catch::Approx(5.0f));
    REQUIRE(info.encoder_info.detection_length == Catch::Approx(12.4f));
    REQUIRE(info.encoder_info.headroom == Catch::Approx(8.0f));
    REQUIRE(info.encoder_info.min_headroom == Catch::Approx(4.2f));
}

TEST_CASE("Happy Hare: partial encoder object defaults missing fields", "[ams][happy_hare][clog]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data;
    mmu_data["encoder"] = {{"flow_rate", 92}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.encoder_info.flow_rate == 92);
    // Not enabled because clog_detection_enabled not set
    REQUIRE(info.encoder_info.enabled == false);
    REQUIRE(info.encoder_info.detection_length == Catch::Approx(0.0f));
    REQUIRE(info.encoder_info.headroom == Catch::Approx(0.0f));
}

TEST_CASE("Happy Hare: a frame without encoder, flowguard or tangle objects leaves defaults",
          "[ams][happy_hare][clog]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.test_parse_mmu_state(nlohmann::json{{"action", "Idle"}});

    const auto info = helper.get_system_info();
    CHECK_FALSE(info.encoder_info.enabled);
    CHECK(info.encoder_info.flow_rate == -1);
    CHECK_FALSE(info.flowguard_info.enabled);
    CHECK_FALSE(info.flowguard_info.active);
    CHECK(info.flowguard_info.trigger.empty());
    CHECK(info.flowguard_info.reason.empty());
    CHECK_FALSE(info.tangle_prevention.present);
}

TEST_CASE("Happy Hare: parse flowguard object", "[ams][happy_hare][clog]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data;
    mmu_data["flowguard"] = {{"enabled", true}, {"active", true},  {"trigger", "CLOG"},
                             {"level", 0.35},   {"max_clog", 0.8}, {"max_tangle", -0.6}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.flowguard_info.enabled == true);
    REQUIRE(info.flowguard_info.active == true);
    REQUIRE(info.flowguard_info.trigger == "CLOG");
    REQUIRE(info.flowguard_info.level == Catch::Approx(0.35f));
    REQUIRE(info.flowguard_info.max_clog == Catch::Approx(0.8f));
    REQUIRE(info.flowguard_info.max_tangle == Catch::Approx(-0.6f));
}

TEST_CASE("Happy Hare: v4 flowguard reason and tangle_prevention parse",
          "[ams][happy_hare][clog]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data;
    mmu_data["flowguard"] = {{"enabled", true},
                             {"active", true},
                             {"trigger", "TANGLE"},
                             {"reason", "no movement"},
                             {"level", -1.0}};
    mmu_data["tangle_prevention"] = {{"enabled", true},
                                     {"active", true},
                                     {"boosted", true},
                                     {"threshold", 0.6},
                                     {"release", 0.3}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    CHECK(info.flowguard_info.reason == "no movement");
    CHECK(info.tangle_prevention.present);
    CHECK(info.tangle_prevention.enabled);
    CHECK(info.tangle_prevention.active);
    CHECK(info.tangle_prevention.boosted);
    CHECK(info.tangle_prevention.threshold == Catch::Approx(0.6f));
    CHECK(info.tangle_prevention.release == Catch::Approx(0.3f));

    // Un-trip clears the reason; a later trip without one cannot show it.
    helper.test_parse_mmu_state(nlohmann::json{{"flowguard", {{"trigger", ""}}}});
    CHECK(helper.get_system_info().flowguard_info.reason.empty());
    helper.test_parse_mmu_state(nlohmann::json{{"flowguard", {{"trigger", "CLOG"}}}});
    CHECK(helper.get_system_info().flowguard_info.reason.empty());

    // A unit without a buffer publishes null: the state clears.
    helper.test_parse_mmu_state(nlohmann::json{{"tangle_prevention", nullptr}});
    CHECK_FALSE(helper.get_system_info().tangle_prevention.present);
}

TEST_CASE("Happy Hare: parse sync_feedback_flow_rate", "[ams][happy_hare][clog]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data;
    mmu_data["sync_feedback_flow_rate"] = 92.5;
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.sync_feedback_flow_rate == Catch::Approx(92.5f));
}

TEST_CASE("Happy Hare: VirtualSelector is a HUB, every other selector is LINEAR",
          "[ams][happy_hare][topology]") {
    const std::pair<const char*, PathTopology> rows[] = {
        {"", PathTopology::LINEAR}, // not reported yet
        {"LinearSelector", PathTopology::LINEAR},
        {"RotarySelector", PathTopology::LINEAR},
        {"ServoSelector", PathTopology::LINEAR},
        {"VirtualSelector", PathTopology::HUB},
    };
    for (const auto& [selector, topology] : rows) {
        INFO("selector_type \"" << selector << "\"");
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        if (*selector)
            helper.set_selector_type(selector);
        CHECK(helper.get_topology() == topology);
        // An index with no unit falls back to the machine's topology.
        helper.initialize_test_gates(4);
        CHECK(helper.get_unit_topology(99) == topology);
    }
}

TEST_CASE("Happy Hare: a VirtualSelector makes every unit a HUB with no encoder",
          "[ams][happy_hare][topology]") {
    SECTION("from the status frame, on one unit or two") {
        for (const int units : {1, 2}) {
            INFO("units=" << units);
            helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
            AmsBackendHappyHareTestHelper& helper = *helper_reg;
            helper.set_selector_type("VirtualSelector");
            nlohmann::json mmu_data;
            if (units > 1)
                mmu_data["num_units"] = units;
            mmu_data["gate_status"] = std::vector<int>(4 * units, 1);
            mmu_data["gate_material"] = std::vector<std::string>(4 * units, "PLA");
            mmu_data["gate"] = 0;
            mmu_data["tool"] = 0;
            mmu_data["filament"] = "Unloaded";
            mmu_data["action"] = "Idle";
            helper.test_parse_mmu_state(mmu_data);

            auto info = helper.get_system_info();
            REQUIRE(info.units.size() == static_cast<size_t>(units));
            for (const auto& unit : info.units) {
                CHECK(unit.topology == PathTopology::HUB);
                CHECK_FALSE(unit.has_encoder);
            }
        }
    }
    SECTION("a selector type arriving after the slots updates them") {
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        REQUIRE(helper.get_system_info().units[0].topology == PathTopology::LINEAR);
        REQUIRE(helper.get_system_info().units[0].has_encoder);

        helper.set_selector_type("VirtualSelector");
        helper.apply_selector_type_update();

        auto info = helper.get_system_info();
        CHECK(info.units[0].topology == PathTopology::HUB);
        CHECK_FALSE(info.units[0].has_encoder);
    }
}

// --- Sync feedback bias parsing ---

TEST_CASE("Happy Hare parses sync_feedback_bias fields", "[ams][happy_hare][v4][sync_feedback]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    SECTION("positive bias (compression)") {
        nlohmann::json mmu_data = {{"sync_feedback_bias_modelled", 0.75f},
                                   {"sync_feedback_bias_raw", 0.82f}};
        helper.test_parse_mmu_state(mmu_data);

        auto info = helper.get_system_info();
        REQUIRE(info.sync_feedback_bias == Catch::Approx(0.75f));
        REQUIRE(info.sync_feedback_bias_raw == Catch::Approx(0.82f));
    }

    SECTION("negative bias (tension)") {
        nlohmann::json mmu_data = {{"sync_feedback_bias_modelled", -0.5f},
                                   {"sync_feedback_bias_raw", -0.33f}};
        helper.test_parse_mmu_state(mmu_data);

        auto info = helper.get_system_info();
        REQUIRE(info.sync_feedback_bias == Catch::Approx(-0.5f));
        REQUIRE(info.sync_feedback_bias_raw == Catch::Approx(-0.33f));
    }

    SECTION("missing fields remain at sentinel") {
        nlohmann::json mmu_data = {{"gate_status", {1, 1, 1, 1}}};
        helper.test_parse_mmu_state(mmu_data);

        auto info = helper.get_system_info();
        REQUIRE(info.sync_feedback_bias == Catch::Approx(-2.0f));
        REQUIRE(info.sync_feedback_bias_raw == Catch::Approx(-2.0f));
    }
}

TEST_CASE("Happy Hare offers each setting in its section", "[ams][happy_hare][device_actions]") {
    using helix::printer::ActionType;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto actions = helper.get_device_actions();
    auto find = [&](const std::string& id) -> const helix::printer::DeviceAction* {
        for (const auto& a : actions) {
            if (a.id == id)
                return &a;
        }
        return nullptr;
    };

    struct Row {
        const char* id;
        const char* section;
        std::optional<ActionType> type;
        double shipped; // value before the config loads (< 0 = not checked)
    };
    const Row rows[] = {
        {"gear_from_buffer_speed", "speed", std::nullopt, 150.0},
        {"gear_from_spool_speed", "speed", std::nullopt, 60.0},
        {"extruder_load_speed", "speed", std::nullopt, -1},
        {"extruder_unload_speed", "speed", std::nullopt, -1},
        {"toolhead_sensor_to_nozzle", "toolhead", std::nullopt, -1},
        {"toolhead_extruder_to_nozzle", "toolhead", std::nullopt, -1},
        {"toolhead_entry_to_extruder", "toolhead", std::nullopt, -1},
        {"toolhead_ooze_reduction", "toolhead", std::nullopt, -1},
        {"sync_to_extruder", "accessories", ActionType::TOGGLE, -1},
        {"test_move", "maintenance", ActionType::BUTTON, -1},
    };
    for (const auto& row : rows) {
        INFO(row.id);
        const auto* a = find(row.id);
        REQUIRE(a != nullptr);
        CHECK(a->section == row.section);
        if (row.type)
            CHECK(a->type == *row.type);
        if (row.shipped >= 0)
            CHECK(std::any_cast<double>(a->current_value) == Catch::Approx(row.shipped));
    }
    for (const char* servo : {"servo_up", "servo_move", "servo_down"}) {
        INFO(servo);
        CHECK(find(servo) != nullptr);
    }
    // The single gear speed is split in two; the servo has no calibration.
    CHECK(find("gear_load_speed") == nullptr);
    CHECK(find("calibrate_servo") == nullptr);
}

TEST_CASE("get_device_actions shows the configured and live values",
          "[ams][happy_hare][device_actions]") {
    using Value = std::variant<double, bool, std::string>;
    struct Row {
        const char* id;
        nlohmann::json frame; // status first sent (null = none)
        Value value;
    };
    const Row rows[] = {
        {"gear_from_buffer_speed", nullptr, 180.0},
        {"gear_unload_speed", nullptr, 90.0},
        {"sync_to_extruder", nullptr, false},
        {"clog_detection", {{"flowguard", {{"encoder_mode", 2}}}}, std::string("Auto")},
        {"led_mode",
         {{"leds", {{"unit0", {{"exit_effect", "breathing"}}}}}},
         std::string("breathing")},
        {"espooler_mode", {{"espooler_active", "assist"}}, std::string("assist")},
    };
    for (const auto& row : rows) {
        INFO(row.id);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.set_config_defaults_for_test();
        if (!row.frame.is_null())
            helper.test_parse_mmu_state(row.frame);

        const auto actions = helper.get_device_actions();
        const auto it = std::find_if(actions.begin(), actions.end(),
                                     [&](const auto& a) { return a.id == row.id; });
        REQUIRE(it != actions.end());
        if (const auto* d = std::get_if<double>(&row.value))
            CHECK(std::any_cast<double>(it->current_value) == Catch::Approx(*d));
        else if (const auto* b = std::get_if<bool>(&row.value))
            CHECK(std::any_cast<bool>(it->current_value) == *b);
        else
            CHECK(std::any_cast<std::string>(it->current_value) ==
                  std::get<std::string>(row.value));
    }
}

TEST_CASE("get_device_actions waits for the config only on config-backed settings",
          "[ams][happy_hare][device_actions]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    // Do NOT call set_config_defaults_for_test() — config_defaults_.loaded is false

    auto actions = helper.get_device_actions();
    for (const char* id : {"gear_from_buffer_speed", "sync_to_extruder", "clog_detection"}) {
        INFO(id);
        const auto* a = find_action(actions, id);
        REQUIRE(a != nullptr);
        CHECK_FALSE(a->enabled);
        CHECK(a->disable_reason == "Loading configuration...");
    }
    for (const char* id : {"gear_sync", "motors_toggle", "led_mode", "espooler_mode"}) {
        INFO(id);
        const auto* a = find_action(actions, id);
        REQUIRE(a != nullptr);
        CHECK(a->enabled);
    }

    // A live command runs; a config-backed setting is refused until the config loads.
    REQUIRE(helper.execute_device_action("gear_sync", std::any(true)).success());
    CHECK(helper.has_gcode("MMU_SYNC_GEAR_MOTOR SYNC=1"));
    helper.clear_captured_gcodes();
    CHECK(helper.execute_device_action("gear_from_buffer_speed", std::any(200.0f)).result ==
          AmsResult::WRONG_STATE);
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("clog detection shows the encoder's reported mode while it waits for the config",
          "[ams][happy_hare][device_actions]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    // config_defaults_.loaded stays false: the configfile has not arrived.

    nlohmann::json mmu_data;
    mmu_data["flowguard"] = {{"encoder_mode", 2}};
    helper.test_parse_mmu_state(mmu_data);

    const auto actions = helper.get_device_actions();
    const auto it = std::find_if(actions.begin(), actions.end(),
                                 [](const auto& a) { return a.id == "clog_detection"; });
    REQUIRE(it != actions.end());
    CHECK_FALSE(it->enabled);
    CHECK(it->disable_reason == "Loading configuration...");
    const auto* shown = std::any_cast<std::string>(&it->current_value);
    REQUIRE(shown != nullptr);
    CHECK(*shown == "Auto");
}

TEST_CASE("A VirtualSelector disables the servo, selector and encoder actions",
          "[ams][happy_hare][device_actions]") {
    for (const bool type_b : {true, false}) {
        INFO((type_b ? "VirtualSelector" : "default selector"));
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.set_config_defaults_for_test();
        if (type_b)
            helper.set_selector_type("VirtualSelector");

        const auto actions = helper.get_device_actions();
        for (const char* id : {"servo_buzz", "calibrate_encoder", "servo_up", "servo_move",
                               "servo_down", "selector_speed", "clog_detection"}) {
            INFO(id);
            const auto* a = find_action(actions, id);
            REQUIRE(a != nullptr);
            CHECK(a->enabled == !type_b);
            if (type_b)
                CHECK_FALSE(a->disable_reason.empty());
        }
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a Happy Hare speed slider in the section overlay opens at its value and applies",
                 "[ams][happy_hare][device_actions][device-section]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.set_config_defaults_for_test();

    auto& overlay = helix::ui::get_ams_device_section_detail_overlay();
    overlay.init_subjects();
    overlay.show(test_screen(), "speed", "Speed");
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(10);

    lv_obj_t* slider =
        helix::test::find_device_action_slider(overlay.get_root(), "gear_from_buffer_speed");
    REQUIRE(slider != nullptr);
    CHECK(lv_slider_get_value(slider) == 180);

    lv_slider_set_value(slider, 200, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
    CHECK(helper.has_gcode_containing("GEAR_FROM_BUFFER_SPEED=200"));

    NavigationManager::instance().go_back();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(10);
    helix::ui::destroy_static_panels();
}

TEST_CASE("reapply_overrides batches into single MMU_TEST_CONFIG command",
          "[ams][happy_hare][device_actions]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.set_config_defaults_for_test();

    // Set multiple overrides
    helper.execute_device_action("gear_from_buffer_speed", std::any(200.0));
    helper.execute_device_action("extruder_load_speed", std::any(50.0));
    helper.captured_gcodes.clear();

    // Reapply should batch all overrides
    helper.test_reapply_overrides();

    // One command per parameter: v3 refuses a whole MMU_TEST_CONFIG over any
    // one name it does not take.
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.has_gcode("MMU_TEST_CONFIG GEAR_FROM_BUFFER_SPEED=200"));
    REQUIRE(helper.has_gcode_containing("EXTRUDER_LOAD_SPEED=50"));
}

TEST_CASE("Happy Hare selector and gate-check commands, and their refusals",
          "[ams][happy_hare][device_actions]") {
    // move_selector jogs from the current gate (none counts as gate 0) and
    // clamps to the gates that exist.
    enum class Op { Select, Move, Check, CheckAll };
    struct Row {
        const char* name;
        Op op;
        bool running;
        int current;
        int arg;
        const char* gcode; // "" = refused, nothing sent
    };
    const Row rows[] = {
        {"select gate 2", Op::Select, true, -1, 2, "MMU_SELECT GATE=2"},
        {"select out of range", Op::Select, true, -1, 99, ""},
        {"select when stopped", Op::Select, false, -1, 0, ""},
        {"jog forward", Op::Move, true, 1, 1, "MMU_SELECT GATE=2"},
        {"jog back", Op::Move, true, 1, -1, "MMU_SELECT GATE=0"},
        {"jog back from the first gate", Op::Move, true, 0, -1, "MMU_SELECT GATE=0"},
        {"jog forward from the last gate", Op::Move, true, 3, 1, "MMU_SELECT GATE=3"},
        {"jog with no gate selected", Op::Move, true, -1, 1, "MMU_SELECT GATE=1"},
        {"jog when stopped", Op::Move, false, 1, 1, ""},
        {"check gate 1", Op::Check, true, -1, 1, "MMU_CHECK_GATE GATE=1"},
        {"check when stopped", Op::Check, false, -1, 0, ""},
        {"check all", Op::CheckAll, true, -1, 0, "MMU_CHECK_GATE"},
        {"check all when stopped", Op::CheckAll, false, -1, 0, ""},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.set_running(row.running);
        helper.set_current_slot(row.current);

        AmsError err;
        switch (row.op) {
        case Op::Select:
            err = helper.select_gate(row.arg);
            break;
        case Op::Move:
            err = helper.move_selector(row.arg);
            break;
        case Op::Check:
            err = helper.check_gate(row.arg);
            break;
        case Op::CheckAll:
            err = helper.check_all_gates();
            break;
        }
        if (*row.gcode) {
            CHECK(err.result == AmsResult::SUCCESS);
            CHECK(helper.has_gcode(row.gcode));
        } else {
            CHECK_FALSE(err.success());
            CHECK(helper.captured_gcodes.empty());
        }
    }
}

// reset() sends bare MMU_HOME, and with no FORCE_UNLOAD parameter Happy Hare
// runs its unload sequence before homing the selector whenever filament_pos is
// not UNLOADED. That is the whole reason the sidebar may grey Reset here and
// nowhere else, so the capability has to answer true (prestonbrown/helixscreen#1523).
TEST_CASE("Happy Hare declares that reset moves filament", "[ams][happy_hare][capability]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    REQUIRE(helper.reset_moves_filament());
    // The command the capability describes.
    helper.initialize_test_gates(4);
    helper.set_running(true);
    REQUIRE(helper.reset().success());
    REQUIRE(helper.has_gcode("MMU_HOME"));
}

TEST_CASE("Happy Hare config sync action is relabeled, distinct from runtime",
          "[ams][happy_hare][device_actions]") {
    using namespace helix::printer;
    auto actions = hh_default_actions();
    const DeviceAction* config_sync = nullptr;
    const DeviceAction* runtime_sync = nullptr;
    for (const auto& a : actions) {
        if (a.id == "sync_to_extruder")
            config_sync = &a;
        if (a.id == "gear_sync")
            runtime_sync = &a;
    }
    REQUIRE(config_sync != nullptr);
    REQUIRE(runtime_sync != nullptr);
    REQUIRE(config_sync->label == "Sync during printing");
    REQUIRE(runtime_sync->label == "Gear motor synced");
    REQUIRE(runtime_sync->section == "maintenance");
}

TEST_CASE("Happy Hare toolchange_phase_template: ops declare ordered phases",
          "[ams][happy_hare][narration]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    auto swap = hh.toolchange_phase_template(StepOperationType::LOAD_SWAP);
    REQUIRE_FALSE(swap.empty());
    CHECK(swap.front().id == "heat");
    CHECK(swap.back().id == "load");
    // Task 3 maps AmsAction → these exact positions; lock them in.
    REQUIRE(swap.size() == 8);
    CHECK(swap[5].id == "feed");
    CHECK(swap[6].id == "purge");
    // Fresh load skips the unload/cut phases.
    auto fresh = hh.toolchange_phase_template(StepOperationType::LOAD_FRESH);
    REQUIRE_FALSE(fresh.empty());
    bool fresh_has_unload = false;
    for (const auto& p : fresh)
        if (p.id == "unload")
            fresh_has_unload = true;
    CHECK_FALSE(fresh_has_unload);
    // Unload op ends at "unload".
    auto unload = hh.toolchange_phase_template(StepOperationType::UNLOAD);
    REQUIRE_FALSE(unload.empty());
    CHECK(unload.back().id == "unload");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Happy Hare narration: action transitions advance the step subject",
                 "[ams][happy_hare][narration][ui_integration]") {
    auto& ams = helix::AmsState::instance();
    ams.init_subjects(true);
    ams.set_active_step_operation(StepOperationType::LOAD_SWAP);
    ams.set_narration_phase(-1, "");

    auto hh = std::make_unique<AmsBackendHappyHareTestHelper>();
    hh->initialize_test_gates(4);
    auto* hh_raw = hh.get();
    ams.set_backend(std::move(hh)); // backend now reachable for the index subject

    auto feed_action = [&](const char* action) {
        nlohmann::json mmu;
        mmu["action"] = action;
        hh_raw->test_parse_mmu_state(mmu);
        helix::ui::UpdateQueue::instance().drain(); // flush the deferred set
    };

    feed_action("Heating");
    CHECK(lv_subject_get_int(ams.get_toolchange_step_subject()) == 0); // "heat" = index 0

    feed_action("Loading");
    // "feed" is index 5 in the LOAD_SWAP template.
    CHECK(lv_subject_get_int(ams.get_toolchange_step_subject()) == 5);

    feed_action("Purging");
    CHECK(lv_subject_get_int(ams.get_toolchange_step_subject()) == 6); // "purge" = index 6

    ams.set_backend(nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Happy Hare narration: a phase outside the current template lands when the "
                 "operation settles",
                 "[ams][happy_hare][narration][ui_integration]") {
    // The sidebar can start a toolchange as UNLOAD and upgrade it to LOAD_SWAP once
    // it learns a load follows. A phase reported in between belongs to the operation
    // the step bar settles on.
    auto& ams = helix::AmsState::instance();
    ams.init_subjects(true);
    ams.set_active_step_operation(StepOperationType::LOAD_SWAP);
    ams.set_narration_phase(-1, "");
    ams.set_active_step_operation(StepOperationType::UNLOAD);

    auto hh = std::make_unique<AmsBackendHappyHareTestHelper>();
    hh->initialize_test_gates(4);
    auto* hh_raw = hh.get();
    ams.set_backend(std::move(hh));

    nlohmann::json mmu;
    mmu["action"] = "Selecting";
    hh_raw->test_parse_mmu_state(mmu);
    helix::ui::UpdateQueue::instance().drain();
    // UNLOAD has no "select" step, so nothing lights yet.
    CHECK(lv_subject_get_int(ams.get_toolchange_step_subject()) == -1);

    ams.set_active_step_operation(StepOperationType::LOAD_SWAP);
    CHECK(lv_subject_get_int(ams.get_toolchange_step_subject()) == 4); // "select" = index 4

    ams.set_narration_phase(-1, "");
    ams.set_backend(nullptr);
}

// ============================================================================
// Override store — user identity Happy Hare's gate map cannot hold
// ============================================================================
//
// Same rationale as AFC (see test_ams_backend_afc.cpp): Happy Hare's gate map
// carries spool_id / material / colour, but not brand, spool_name,
// total_weight_g, colour name or the Spoolman filament+vendor ids. Those live
// only in the override store, and the store must use a PRIVATE namespace
// because lane_data belongs to the Happy Hare plugin itself.
//
// NOTE: written blind — there is no Happy Hare hardware on hand. It deliberately
// mirrors the AFC integration rather than inventing anything.

TEST_CASE("an outside re-bind takes the old spool's brand off a Happy Hare gate",
          "[ams][happyhare][lane][1672]") {
    // Something outside HelixScreen - Mainsail, the MMU's own screen, a macro -
    // points a gate at a different spool. The gate map is the only thing Happy
    // Hare states about a binding, so that reading retires the records
    // describing the old spool, and parse_mmu_state paints the gate from what
    // is left before the frame ends. A brand only the old spool's record ever
    // carried has nothing to restate it, and apply_resolved() leaves a field no
    // source observed alone, so without the retire the incoming spool wears the
    // outgoing one's brand.
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    SpoolInfo spool;
    spool.id = 42;
    spool.vendor = "Polymaker";
    spool.filament_name = "PolyLite PETG";
    spool.material = "PETG";
    spool.color_hex = "FF00FF";
    spool.filament_id = 55;
    helix::test::spool_states(helper, 1, spool);
    helper.repaint_slot_from_lane(1);
    REQUIRE(helper.get_slot_info(1).brand == "Polymaker");
    REQUIRE(helper.get_slot_info(1).spoolman_filament_id == 55);

    // Gate 2 is the control: same shape, and the frame below reports the spool
    // its lane already names, so its binding holds and nothing is retired.
    SpoolInfo held;
    held.id = 7;
    held.vendor = "Sunlu";
    held.filament_name = "PLA Meta";
    held.material = "PLA";
    held.color_hex = "00FF00";
    helix::test::spool_states(helper, 2, held);
    helper.repaint_slot_from_lane(2);
    REQUIRE(helper.get_slot_info(2).brand == "Sunlu");

    // Gate 1 now reports a spool its lane does not describe: a re-bind, which
    // needs no setting turned on, unlike the eject verdict. Gate 2 reports the
    // spool it already had.
    helper.feed_mmu_gate_spool_ids({0, 99, 7, 0});

    CHECK(helper.get_slot_info(1).brand.empty());
    CHECK(helper.get_slot_info(1).spoolman_filament_id == 0);
    CHECK(helper.get_slot_info(2).brand == "Sunlu");
}

TEST_CASE("a spool Spoolman denies keeps the Happy Hare gate's brand as remembered",
          "[ams][happy_hare][happyhare][lane][1672]") {
    // A spool deleted in Spoolman is bookkeeping, not a spool change: the gate
    // goes on showing what is loaded. The manager's denial path drops the
    // record and files what the gate showed as remembered, the way an unlink
    // that kept the identity does, so the identity stands until an edit or a
    // spool change replaces it. Both lane shapes - one no one edited and one
    // carrying a person's own pick - must end up showing the same brand.
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    SpoolInfo spool;
    spool.id = 42;
    spool.vendor = "Polymaker";
    spool.filament_name = "PolyLite PETG";
    spool.material = "PETG";
    spool.color_hex = "FF00FF";
    helix::test::spool_states(helper, 1, spool);
    helper.repaint_slot_from_lane(1);
    REQUIRE(helper.get_slot_info(1).brand == "Polymaker");

    SECTION("an unedited gate") {
        helix::test::spool_denied_on_lane(helper, 1);
        helper.repaint_slot_from_lane(1);
        CHECK(helper.get_slot_info(1).brand == "Polymaker");

        // The next gate FRAME must not retire it either: each frame starts
        // from what the lane holds now, and the lane's remembered record
        // states it.
        helper.feed_mmu_gate_status({2, 2, 2, 2});
        CHECK(helper.get_slot_info(1).brand == "Polymaker");
    }

    SECTION("a gate a person picked a colour on") {
        // The editor opens on the lane's current state and moves one field;
        // the brand stays the server's word while the record stands.
        SlotInfo edit = helper.get_slot_info(1);
        edit.color_rgb = 0x1A73E8;
        edit.color_name = "Sky Blue";
        REQUIRE(helix::test::apply_edit(helper, 1, edit).success());
        REQUIRE(helper.has_gate_override(1));

        helix::test::spool_denied_on_lane(helper, 1);
        helper.repaint_slot_from_lane(1);
        const SlotInfo after = helper.get_slot_info(1);
        CHECK(after.brand == "Polymaker");
        // The person's own pick outranks what is remembered, denial or not.
        CHECK(after.color_name == "Sky Blue");
    }
}

TEST_CASE("a Spoolman record re-filed without its brand clears the Happy Hare gate's brand",
          "[ams][happy_hare][happyhare][lane][1672]") {
    // The answer CHANGING is the other half of the contract: a re-filed
    // record replaces the old one whole, so a server that stopped stating a
    // brand leaves nothing standing over it, and the gate's SlotInfo - which
    // persists across frames - must let the brand go rather than keep showing
    // what an earlier paint wrote.
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    SpoolInfo spool;
    spool.id = 42;
    spool.vendor = "Polymaker";
    spool.filament_name = "PolyLite PETG";
    spool.material = "PETG";
    spool.color_hex = "FF00FF";
    helix::test::spool_states(helper, 1, spool);
    helper.repaint_slot_from_lane(1);
    REQUIRE(helper.get_slot_info(1).brand == "Polymaker");

    // The server re-files the spool with no vendor and no spool name: the
    // record that lands says nothing about either field. With the repaint the
    // manager runs, nothing states a brand any more.
    SpoolInfo brandless = spool;
    brandless.vendor.clear();
    brandless.filament_name.clear();
    helix::test::spool_states(helper, 1, brandless);
    helper.repaint_slot_from_lane(1);
    CHECK(helper.get_slot_info(1).brand.empty());

    // Restate the record, then re-file it brandless again and let the next
    // gate FRAME be the first thing that runs: each frame starts from what
    // the lane holds now, not from what the struct still carries.
    helix::test::spool_states(helper, 1, spool);
    helper.repaint_slot_from_lane(1);
    REQUIRE(helper.get_slot_info(1).brand == "Polymaker");
    helix::test::spool_states(helper, 1, brandless);
    helper.feed_mmu_gate_status({2, 2, 2, 2});
    CHECK(helper.get_slot_info(1).brand.empty());

    // A record that states the brand again paints it back, so a server-side
    // correction still lands.
    helix::test::spool_states(helper, 1, spool);
    helper.feed_mmu_gate_status({2, 2, 2, 2});
    CHECK(helper.get_slot_info(1).brand == "Polymaker");
}

TEST_CASE("HappyHare override survives a gate-map update that omits identity",
          "[ams][happyhare][override]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    SlotInfo info = helper.get_slot_info(0);
    info.brand = "Polymaker";
    info.spool_name = "PolyLite Grey";
    info.spoolman_id = 42;
    info.spoolman_filament_id = 55;
    info.total_weight_g = 1000.0f;
    helix::test::edit_slot_as_user(helper, 0, info);
    REQUIRE(helper.get_slot_info(0).spoolman_filament_id == 55);

    // A gate-map refresh that clears the spool id upstream.
    helper.feed_mmu_gate_spool_ids({0, 0, 0, 0});

    const SlotInfo after = helper.get_slot_info(0);
    CHECK(after.brand == "Polymaker");
    CHECK(after.spool_name == "PolyLite Grey");
    CHECK(after.spoolman_id == 42);
    CHECK(after.spoolman_filament_id == 55);
    CHECK(after.total_weight_g == Catch::Approx(1000.0f));
}

TEST_CASE("Happy Hare's own gate writes carry the filament id", "[ams][happyhare][1632]") {
    // The lane repaint that closes a full commit restates the field from the
    // override, so an edit-driven assertion cannot tell the backend's own copy
    // from the restatement. Both pins sit below any repaint: apply_user_edit()
    // writes the gate and returns before commit_user_edit() repaints, and the
    // retire runs here with no frame behind it at all.
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);

    SECTION("the editor write copies it into the gate") {
        SlotInfo info = helper.get_slot_info(0);
        info.spoolman_id = 42;
        info.spoolman_filament_id = 55;
        const AmsError err = helper.apply_user_edit(
            0, info, helix::ams::user_edit_observation(helper.get_slot_info(0), info));
        REQUIRE(err.success());
        REQUIRE(helper.get_slot_info(0).spoolman_filament_id == 55);
    }

    SECTION("a departed binding's retire drops it") {
        helper.set_gate_spoolman_link(0, 42, 55, 3);
        REQUIRE(helper.get_slot_info(0).spoolman_filament_id == 55);
        // The retire expects its caller to hold mutex_; get_slot_info takes
        // the same lock itself, so release before reading back.
        {
            std::lock_guard<std::mutex> lock(helix::HappyHareTestAccess::mutex(helper));
            helix::HappyHareTestAccess::retire_departed_identity(helper, 0);
        }
        CHECK(helper.get_slot_info(0).spoolman_filament_id == 0);
        CHECK(helper.get_slot_info(0).spoolman_vendor_id == 0);
    }
}

TEST_CASE("HappyHare clear_slot_override drops the retained identity",
          "[ams][happyhare][override][1625]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    SlotInfo info;
    info.brand = "Polymaker";
    info.spoolman_id = 42;
    helix::test::apply_edit(helper, 0, info);
    helper.set_gate_spoolman_link(0, 42, 77, 3);
    REQUIRE(helper.get_slot_info(0).spoolman_filament_id == 77);

    helper.clear_slot_override(0);

    const SlotInfo after = helper.get_slot_info(0);
    CHECK(after.brand.empty());
    CHECK(after.spoolman_id == 0);
    // All three handles die together: a surviving filament handle would keep
    // naming a Spoolman record the cleared slot is no longer linked to.
    CHECK(after.spoolman_filament_id == 0);
    CHECK(after.spoolman_vendor_id == 0);
}

TEST_CASE("HappyHare persist_override records pure black but not the no-color sentinel",
          "[ams][happyhare][override]") {
    struct Row {
        const char* name;
        uint32_t color;
        bool recorded;
    };
    const Row rows[] = {
        {"a deliberate pure black", 0x000000, true},
        {"the no-color sentinel", helix::kAmsDefaultSlotColor, false},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);

        SlotInfo info;
        info.material = "PLA";
        info.color_rgb = row.color;
        helix::test::apply_edit(helper, 0, info);

        REQUIRE(helper.has_gate_override(0));
        const auto& ovr = helix::HappyHareTestAccess::overrides(helper).at(0);
        CHECK(ovr.color_set == row.recorded);
        if (row.recorded)
            CHECK(ovr.color_rgb == row.color);
        CHECK(ovr.material == "PLA");
    }
}

TEST_CASE("HappyHare persist_override wires nozzle/bed temps into the override",
          "[ams][happyhare][override]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    SlotInfo info;
    info.material = "PETG";
    info.bed_temp = 80;
    info.nozzle_temp_min = 230;
    info.nozzle_temp_max = 250;
    helix::test::apply_edit(helper, 0, info);

    REQUIRE(helper.has_gate_override(0));
    const auto& ovr = helix::HappyHareTestAccess::overrides(helper).at(0);
    CHECK(ovr.bed_temp == 80);
    CHECK(ovr.nozzle_temp == 240); // midpoint of min/max
}

// ============================================================================
// Bypass: Happy Hare has to answer for itself, same as AFC (#1229)
// ============================================================================
//
// The twin of "AFC enable_bypass refuses while filament is at the toolhead" in
// test_ams_backend_afc.cpp. allows_implicit_chaining() == false means the
// sidebar sends one command and lets the backend refuse; execute_gcode() is
// fire-and-forget (returns success before Klipper answers), so without this
// precondition MMU_SELECT_BYPASS reports success and changes nothing.
//
// Happy Hare's own cmd_MMU_SELECT_BYPASS runs check_if_loaded(), which refuses
// unless filament_pos is UNLOADED (0) or UNKNOWN (-1) and answers only with a
// `!!` line. Mirroring that exact predicate — rather than system_info_
// .filament_loaded, which is set solely from filament == "Loaded" — is what
// makes an intermediate position refuse here instead of silently no-opping.

TEST_CASE("Happy Hare enable_bypass refuses unless the filament is parked",
          "[ams][happy_hare][bypass][1229]") {
    auto mmu_at = [](int filament_pos, const char* filament_state) {
        return nlohmann::json{{"gate", 0},
                              {"tool", 0},
                              {"filament", filament_state},
                              {"action", "Idle"},
                              {"has_bypass", true},
                              {"filament_pos", filament_pos},
                              {"gate_status", {1, 0, 2, 1}},
                              {"ttg_map", {0, 1, 2, 3}}};
    };

    SECTION("loaded to the nozzle refuses and sends nothing") {
        AmsBackendHappyHareTestHelper helper;
        helper.set_running(true);
        helper.initialize_test_gates(4);
        helper.test_parse_mmu_state(mmu_at(10, "Loaded")); // FILAMENT_POS_LOADED
        helper.clear_captured_gcodes();

        AmsError err = helper.enable_bypass();

        REQUIRE(err.result == AmsResult::WRONG_STATE);
        REQUIRE_FALSE(err.user_msg.empty());
        REQUIRE(helper.captured_gcodes.empty());
    }

    // The case system_info_.filament_loaded would miss: mid-bowden is not
    // "Loaded", but Happy Hare still refuses, so we must too.
    SECTION("mid-bowden refuses even though filament is not 'Loaded'") {
        AmsBackendHappyHareTestHelper helper;
        helper.set_running(true);
        helper.initialize_test_gates(4);
        helper.test_parse_mmu_state(mmu_at(3, "Unloading")); // FILAMENT_POS_IN_BOWDEN
        helper.clear_captured_gcodes();

        AmsError err = helper.enable_bypass();

        REQUIRE(err.result == AmsResult::WRONG_STATE);
        REQUIRE(helper.captured_gcodes.empty());
    }

    SECTION("parked at the gate sends MMU_SELECT_BYPASS") {
        AmsBackendHappyHareTestHelper helper;
        helper.set_running(true);
        helper.initialize_test_gates(4);
        helper.test_parse_mmu_state(mmu_at(0, "Unloaded")); // FILAMENT_POS_UNLOADED
        helper.clear_captured_gcodes();

        AmsError err = helper.enable_bypass();

        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(std::find(helper.captured_gcodes.begin(), helper.captured_gcodes.end(),
                          "MMU_SELECT_BYPASS") != helper.captured_gcodes.end());
    }
}

// ============================================================================
// Happy Hare v4 machine-field layout
//
// v3 puts filament_heater / environment_sensor directly on [mmu_machine] in
// configfile.settings. v4 leaves that object holding only happy_hare_version
// and units, and publishes the real values per unit in the LIVE mmu_machine
// status object. Reading configfile alone finds a populated-looking object with
// none of the fields in it, so the miss is silent: no warning, no log, just an
// environment indicator that never fills in.
// ============================================================================

TEST_CASE("Happy Hare v4 takes machine fields from live status, not configfile",
          "[ams][happy_hare][1479]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    // Exactly the shape a v4 machine reports: configfile carries the version and
    // the unit list and nothing else usable.
    nlohmann::json configfile_settings = {
        {"mmu_machine",
         {{"happy_hare_version", "4.0.0"}, {"units", nlohmann::json::array({"unit0"})}}}};

    nlohmann::json live_mmu_machine = {{"happy_hare_version", "4.0.0"},
                                       {"num_units", 1},
                                       {"unit_0",
                                        {{"name", "unit0"},
                                         {"environment_sensor", "aht10 unit0_Env"},
                                         {"filament_heater", "heater_generic box1_heater"}}}};

    helper.test_apply_heater_config(configfile_settings, live_mmu_machine);
    helper.test_parse_mmu_state({{"drying_state", nlohmann::json::array({"", "", "", ""})}});

    // Feeding a status keyed on the live name is the proof the name resolved:
    // nothing surfaces unless environment_sensor_name_ was assigned from unit_0.
    helper.test_apply_environment_sensor_status(
        {{"aht10 unit0_Env", {{"temperature", 26.5}, {"humidity", 41.0}}}});

    auto info = helper.get_system_info();
    REQUIRE_FALSE(info.units.empty());
    REQUIRE(info.units[0].environment.has_value());
    CHECK(info.units[0].environment->temperature_c == Catch::Approx(26.5f));
    CHECK(info.units[0].environment->has_humidity);
    CHECK(info.units[0].environment->humidity_pct == Catch::Approx(41.0f));
}

TEST_CASE("Happy Hare v4 resolves the filament heater from live status",
          "[ams][happy_hare][1479]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    nlohmann::json configfile_settings = {
        {"mmu_machine",
         {{"happy_hare_version", "4.0.0"}, {"units", nlohmann::json::array({"unit0"})}}},
        {"mmu_unit_parameters unit0",
         {{"heater_max_temp", 65.0}, {"heater_default_dry_humidity", 30.0}}}};

    nlohmann::json live_mmu_machine = {
        {"happy_hare_version", "4.0.0"},
        {"num_units", 1},
        {"unit_0", {{"name", "unit0"}, {"filament_heater", "heater_generic box1_heater"}}}};

    helper.test_apply_heater_config(configfile_settings, live_mmu_machine);
    helper.test_parse_mmu_state({{"drying_state", nlohmann::json::array({"", "", "", ""})}});
    helper.test_apply_filament_heater_status(
        {{"heater_generic box1_heater", {{"temperature", 52.0}}}});

    auto info = helper.get_system_info();
    REQUIRE_FALSE(info.units.empty());
    REQUIRE(info.units[0].environment.has_value());
    CHECK(info.units[0].environment->temperature_c == Catch::Approx(52.0f));

    // v4 keeps heater_max_temp on the unit's own parameters section.
    CHECK(helper.get_dryer_info().max_temp_c == Catch::Approx(65.0f));
    // The default humidity target sits beside it.
    CHECK(HappyHareTestAccess::dryer_info(helper).default_humidity_target == 30.0f);
}

TEST_CASE("Happy Hare v3 config still resolves when no live unit object exists",
          "[ams][happy_hare][1479]") {
    // The fallback half. A v3 machine publishes no unit_N sub-object, so the
    // resolver has to keep answering from configfile or this change would break
    // every existing install to fix the new one.
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    nlohmann::json configfile_settings = {
        {"mmu_machine", {{"environment_sensor", "bme280 mmu_box"}}}};

    // Live object present but carrying no per-unit fields, which is what a v3
    // machine looks like when the query asks for it anyway.
    nlohmann::json live_mmu_machine = {{"happy_hare_version", "3.3.0"}};

    helper.test_apply_heater_config(configfile_settings, live_mmu_machine);
    helper.test_parse_mmu_state({{"drying_state", nlohmann::json::array({"", "", "", ""})}});
    helper.test_apply_environment_sensor_status(
        {{"bme280 mmu_box", {{"temperature", 24.5}, {"humidity", 38.0}}}});

    auto info = helper.get_system_info();
    REQUIRE_FALSE(info.units.empty());
    REQUIRE(info.units[0].environment.has_value());
    CHECK(info.units[0].environment->temperature_c == Catch::Approx(24.5f));
    CHECK(info.units[0].environment->humidity_pct == Catch::Approx(38.0f));
}

TEST_CASE("Happy Hare offers a dryer only with a heater, which takes a live temperature",
          "[ams][happy_hare][v4][dryer][emu][capability]") {
    // Happy Hare's environment manager always publishes drying_state, so a
    // passive box with only sensors must not offer drying (MMU_HEATER would be
    // refused with "No MMU heater configured"). MMU_HEATER TEMP= retargets a
    // running cycle; TIMER is read only on DRY=1, which is refused mid-cycle,
    // so the duration cannot change live, whichever form the heater config takes.
    using J = nlohmann::json;
    struct Row {
        const char* name;
        J machine;
        bool supported;
    };
    const Row rows[] = {
        {"sensors only",
         J{{"environment_sensors", "temperature_sensor Lane_0, temperature_sensor Lane_1, "
                                   "temperature_sensor Lane_2, temperature_sensor Lane_3"}},
         false},
        {"one shared heater", J{{"filament_heater", "heater_generic MMU_heater"}}, true},
        {"a heater per gate",
         J{{"filament_heaters", "heater_generic Lane_0_heater, heater_generic Lane_1_heater"}},
         true},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        helper.test_apply_heater_config({{"mmu_machine", row.machine}});
        helper.test_parse_mmu_state({{"drying_state", J::array({"", "", "", ""})}});

        const auto d = helper.get_dryer_info();
        CHECK(d.supported == row.supported);
        if (row.supported) {
            CHECK(d.supports_live_temp);
            CHECK_FALSE(d.supports_live_duration);
        }
    }
}

TEST_CASE("A per-gate drying_state array reaches every gate's zone",
          "[ams][happy_hare][emu][zones]") {
    using namespace helix::printer;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);
    helper.test_apply_heater_config(
        {{"mmu_machine",
          {{"filament_heaters", "heater_generic Lane_0_heater, heater_generic Lane_1_heater"},
           {"environment_sensors", "temperature_sensor Lane_0, temperature_sensor Lane_1"}}}});
    // The second gate's state must survive scanning past the first: stopping at
    // the first hit would leave every later gate unrecorded.
    helper.test_parse_mmu_state({{"drying_state", nlohmann::json::array({"queued", "active"})}});

    const auto zones = helper.get_environment_zones();

    REQUIRE(zones.size() == 2);
    CHECK(zones[0].state == ZoneDryingState::Queued);
    CHECK(zones[1].state == ZoneDryingState::Active);
}

TEST_CASE("A non-string drying_state entry still occupies its gate",
          "[ams][happy_hare][emu][zones]") {
    using namespace helix::printer;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(3);
    helper.test_apply_heater_config(
        {{"mmu_machine",
          {{"filament_heaters", "heater_generic Lane_0_heater, heater_generic Lane_1_heater, "
                                "heater_generic Lane_2_heater"},
           {"environment_sensors", "temperature_sensor Lane_0, temperature_sensor Lane_1, "
                                   "temperature_sensor Lane_2"}}}});
    // A non-string entry between two strings must still occupy its gate slot, or
    // gate 2's state lands on gate 1 instead.
    helper.test_parse_mmu_state(
        {{"drying_state", nlohmann::json::array({"queued", nullptr, "active"})}});

    const auto zones = helper.get_environment_zones();

    REQUIRE(zones.size() == 3);
    CHECK(zones[2].state == ZoneDryingState::Active);
}

TEST_CASE("Happy Hare updates a running dryer: TEMP alone, a restart for a new duration",
          "[ams][happy_hare][dryer][update]") {
    // A bare TEMP= retargets without touching the clock; a new duration needs
    // STOP then DRY=1, since DRY=1 mid-cycle is refused.
    struct Row {
        const char* name;
        float temp;
        int minutes;
        std::vector<const char*> sent;
        std::vector<const char*> never;
    };
    const Row rows[] = {
        {"temperature only", 50.0f, -1, {"MMU_HEATER TEMP=50"}, {"DRY=1", "TIMER=", "STOP=1"}},
        {"a new duration",
         50.0f,
         90,
         {"MMU_HEATER STOP=1", "MMU_HEATER DRY=1 TEMP=50 TIMER=90"},
         {}},
        {"nothing changed", -1, -1, {}, {"MMU_HEATER"}},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.test_apply_heater_config(
            {{"mmu_machine", {{"filament_heater", "heater_generic MMU_heater"}}}});
        helper.clear_captured_gcodes();

        REQUIRE(helper.update_drying(row.temp, row.minutes).success());
        for (const char* g : row.sent)
            CHECK(helper.has_gcode(g));
        for (const char* fragment : row.never)
            CHECK_FALSE(helper.has_gcode_containing(fragment));
        if (row.sent.empty())
            CHECK(helper.captured_gcodes.empty());
    }
}

TEST_CASE("Happy Hare names its positions gates", "[ams][happy_hare][numbering]") {
    helix::AmsBackendHappyHare backend(nullptr, nullptr);
    CHECK(backend.lane_noun() == helix::ui::LaneNoun::Gate);
}

// A persisted override IS a user edit. The lock flags are how any consumer of
// the record tells one from a firmware reading (#965, #1649).
TEST_CASE("Happy Hare marks a persisted edit as the user's own",
          "[ams][happy_hare][filament_slot_override]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    SlotInfo info;
    info.material = "PETG";
    info.color_rgb = 0x1188FF;
    info.color_name = "Blue";
    helix::test::apply_edit(helper, 0, info);

    auto& overrides = helix::HappyHareTestAccess::overrides(helper);
    REQUIRE(overrides.count(0) == 1);
    REQUIRE(overrides.at(0).material == "PETG");
    REQUIRE(overrides.at(0).color_rgb == 0x1188FFu);
    CHECK(helix::ams::declares_color(overrides.at(0)));
    CHECK(helix::ams::declares_material(overrides.at(0)));
}

// ============================================================================
// Maintenance / accessories device actions
// ============================================================================

TEST_CASE("Happy Hare motors toggle turns motors on and off without homing",
          "[ams][happy_hare][device_actions]") {
    AmsBackendHappyHareTestHelper helper;
    helper.set_config_defaults_for_test(); // config-backed actions are offered once it loads

    REQUIRE(helper.execute_device_action("motors_toggle", std::any(true)).success());
    REQUIRE(helper.execute_device_action("motors_toggle", std::any(false)).success());
    CHECK(helper.captured_gcodes == std::vector<std::string>{"MMU_MOTORS_ON", "MMU_MOTORS_OFF"});
}

TEST_CASE("Happy Hare extruder-only load and unload send EXTRUDER_ONLY=1",
          "[ams][happy_hare][device_actions]") {
    AmsBackendHappyHareTestHelper helper;
    helper.set_running(true);
    const auto actions = helper.get_device_actions();

    for (const char* id : {"load_extruder", "unload_extruder"}) {
        INFO(id);
        const auto* a = find_action(actions, id);
        REQUIRE(a != nullptr);
        CHECK(a->section == "maintenance");
        CHECK(a->type == helix::printer::ActionType::BUTTON);
    }

    REQUIRE(helper.execute_device_action("load_extruder").success());
    REQUIRE(helper.execute_device_action("unload_extruder").success());
    CHECK(helper.captured_gcodes ==
          std::vector<std::string>{"MMU_LOAD EXTRUDER_ONLY=1", "MMU_UNLOAD EXTRUDER_ONLY=1"});
}

TEST_CASE("Happy Hare Refresh Spoolman follows HH's spoolman_support",
          "[ams][happy_hare][device_actions]") {
    AmsBackendHappyHareTestHelper helper;

    SECTION("spoolman off: offered disabled, and refused if sent anyway") {
        helper.set_spoolman_mode(SpoolmanMode::OFF);
        const auto actions = helper.get_device_actions();
        const auto* a = find_action(actions, "spoolman_refresh");
        REQUIRE(a != nullptr);
        CHECK(a->section == "accessories");
        CHECK_FALSE(a->enabled);
        CHECK_FALSE(a->disable_reason.empty());
        CHECK_FALSE(helper.execute_device_action("spoolman_refresh").success());
        CHECK(helper.captured_gcodes.empty());
    }
    SECTION("spoolman on") {
        helper.set_spoolman_mode(SpoolmanMode::PULL);
        const auto actions = helper.get_device_actions();
        const auto* a = find_action(actions, "spoolman_refresh");
        REQUIRE(a != nullptr);
        CHECK(a->enabled);
        REQUIRE(helper.execute_device_action("spoolman_refresh").success());
        CHECK(helper.captured_gcodes == std::vector<std::string>{"MMU_SPOOLMAN REFRESH=1"});
    }
}

TEST_CASE("Happy Hare extruder-only load and unload are refused mid-print",
          "[ams][happy_hare][device_actions]") {
    // HH puts no print check on MMU_LOAD/MMU_UNLOAD, and an EXTRUDER_ONLY
    // unload forms a tip and retracts over the part.
    PrintLifecycleFixture fx;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(fx.api.get(),
                                                                             &fx.mock_client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.set_running(true);
    helix::test::set_wire_state(fx.state, helix::PrintJobState::PRINTING);

    CHECK(helper.execute_device_action("load_extruder").result == AmsResult::WRONG_STATE);
    CHECK(helper.execute_device_action("unload_extruder").result == AmsResult::WRONG_STATE);
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("Happy Hare runs only the device actions it offers and has enabled",
          "[ams][happy_hare][device_actions]") {
    AmsBackendHappyHareTestHelper helper;
    CHECK(helper.execute_device_action("not_an_action").result == AmsResult::NOT_SUPPORTED);

    helper.initialize_test_gates(4);
    helper.set_config_defaults_for_test();
    helper.set_selector_type("VirtualSelector"); // no servo on any unit
    const auto actions = helper.get_device_actions();
    const auto* buzz = find_action(actions, "servo_buzz");
    REQUIRE(buzz != nullptr);
    REQUIRE_FALSE(buzz->enabled);
    CHECK(helper.execute_device_action("servo_buzz").result == AmsResult::WRONG_STATE);
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("Happy Hare machine-motion actions are refused while a print holds the printer",
          "[ams][happy_hare][device_actions]") {
    PrintLifecycleFixture fx;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(fx.api.get(),
                                                                             &fx.mock_client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.set_config_defaults_for_test(); // config-backed actions are offered once it loads
    helper.initialize_test_gates(4);
    helper.set_running(true);

    const auto actions = helper.get_device_actions();
    for (const char* id : {"calibrate_bowden", "load_extruder", "test_move"}) {
        const auto* a = find_action(actions, id);
        REQUIRE(a != nullptr);
        CHECK(a->moves_machine);
    }

    for (auto state : {helix::PrintJobState::PRINTING, helix::PrintJobState::PAUSED}) {
        helix::test::set_wire_state(fx.state, state);
        const AmsError refused = helper.execute_device_action("calibrate_bowden");
        CHECK(refused.result == AmsResult::WRONG_STATE);
        CHECK(refused.user_msg == "Printer is busy with a print");
        CHECK(helper.captured_gcodes.empty());
        // An action that leaves the printer still is not the gate's business.
        CHECK(helper.execute_device_action("led_mode", std::any(std::string("off"))).success());
        helper.clear_captured_gcodes();
    }

    // Extruder-only load/unload is how a paused print recovers, so pausing
    // releases it; printing still refuses it.
    const auto* load = find_action(actions, "load_extruder");
    REQUIRE(load != nullptr);
    CHECK(load->allowed_while_paused);
    helix::test::set_wire_state(fx.state, helix::PrintJobState::PRINTING);
    CHECK(helper.execute_device_action("load_extruder").result == AmsResult::WRONG_STATE);
    CHECK(helper.captured_gcodes.empty());
    helix::test::set_wire_state(fx.state, helix::PrintJobState::PAUSED);
    REQUIRE(helper.execute_device_action("load_extruder").success());
    CHECK(helper.has_gcode_containing("MMU_LOAD EXTRUDER_ONLY=1"));
    helper.clear_captured_gcodes();

    helix::test::set_wire_state(fx.state, helix::PrintJobState::STANDBY);
    REQUIRE(helper.execute_device_action("calibrate_bowden").success());
    CHECK(helper.has_gcode_containing("MMU_CALIBRATE_BOWDEN"));
}

// ============================================================================
// Real Happy Hare fault shapes (#1323). HH publishes no Error action: a fault
// in a print is print_state pause_locked plus reason_for_pause, announced by
// `!! MMU issue detected. ...\nReason: <reason>`; outside a print nothing
// pauses and the only signal is `!! MMU issue: <reason>`.
// ============================================================================

namespace {
bool has_recovery(const helix::ErrorEvent& ev, const std::string& tag) {
    for (const auto& a : ev.recovery_actions)
        if (a.log_tag == tag)
            return true;
    return false;
}
} // namespace

TEST_CASE("Happy Hare classify_error: which lines it claims, what it says, how to recover",
          "[ams][happy_hare][error-center][1323]") {
    // Happy Hare's fault shape is print_state pause_locked plus reason_for_pause;
    // its action string has no error value. A paused fault leads with Resume, a
    // standalone one (a failed MMU_LOAD outside a print) offers no Resume and
    // leads with Recover. A recognised keyword alone never claims a line unless
    // the print is paused.
    using J = nlohmann::json;
    const J paused_gate2 = {{"print_state", "pause_locked"},
                            {"reason_for_pause", "Gate 2 is empty!"}};
    const auto paused_line = [](const std::string& reason) {
        return "!! MMU issue detected. Print will be paused\nReason: " + reason;
    };
    struct Row {
        const char* name;
        J frame; // null = none
        bool paused;
        std::string line;
        bool claimed;
        std::string detail; // exact ("" = not checked)
        std::string title;  // "" = not checked
        std::vector<const char*> offers;
        std::vector<const char*> withholds;
        const char* first; // first recovery's log tag ("" = not checked)
    };
    const Row rows[] = {
        {"a generic Klipper error",
         nullptr,
         false,
         "Error: generic klipper error",
         false,
         "",
         "",
         {},
         {},
         ""},
        {"an ok line", nullptr, false, "ok", false, "", "", {}, {}, ""},
        {"!! while paused without a fault",
         nullptr,
         true,
         "!! something unrelated",
         false,
         "",
         "",
         {},
         {},
         ""},
        {"a stale reason while not paused",
         J{{"action", "Idle"},
           {"print_state", "pause_locked"},
           {"reason_for_pause", "Clog detected on gate 0"}},
         false,
         "!! Clog detected",
         false,
         "",
         "",
         {},
         {},
         ""},
        {"an unrelated !! line",
         nullptr,
         false,
         "!! Move out of range: 0.000 400.000 10.000",
         false,
         "",
         "",
         {},
         {},
         ""},
        {"a standalone MMU issue",
         nullptr,
         false,
         "!! MMU issue: Gate 2 is empty!",
         true,
         "Gate 2 is empty!",
         "Filament System Error",
         {"hh::recover", "hh::unlock"},
         {"hh::resume"},
         "hh::recover"},
        {"a paused empty gate",
         J{{"action", "Idle"},
           {"print_state", "pause_locked"},
           {"reason_for_pause", "Gate 2 is empty!"},
           {"filament_pos", 8},
           {"filament", "Loaded"}},
         true,
         paused_line("Gate 2 is empty!"),
         true,
         "Gate 2 is empty!",
         "",
         {"hh::recover", "hh::unload", "hh::unlock"},
         {},
         "hh::resume"},
        {"a paused homing failure",
         J{{"action", "Idle"},
           {"print_state", "pause_locked"},
           {"reason_for_pause", "Failed to home the selector"},
           {"filament_pos", 8},
           {"filament", "Loaded"}},
         true,
         paused_line("Failed to home the selector"),
         true,
         "Failed to home the selector",
         "",
         {"hh::recover", "hh::unload", "hh::unlock"},
         {},
         ""},
        {"a second fault names its own reason",
         paused_gate2,
         true,
         "!! MMU issue detected whilst printer is paused\nReason: Encoder not detecting",
         true,
         "Encoder not detecting",
         "",
         {},
         {},
         ""},
        {"a multi-line reason",
         J{{"print_state", "pause_locked"},
           {"reason_for_pause", "Runout on gate 0\nEndlessSpool is off"}},
         true,
         paused_line("Runout on gate 0\nEndlessSpool is off"),
         true,
         "Runout on gate 0. EndlessSpool is off",
         "Filament runout",
         {},
         {},
         ""},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
        AmsBackendHappyHareTestHelper& hh = *hh_reg;
        hh.initialize_test_gates(4);
        if (!row.frame.is_null())
            hh.test_parse_mmu_state(row.frame);
        helix::ClassifyContext ctx;
        ctx.is_paused = row.paused;

        const auto ev = hh.classify_error(row.line, ctx);
        REQUIRE(ev.has_value() == row.claimed);
        if (!row.claimed)
            continue;
        if (!row.detail.empty())
            CHECK(ev->detail == row.detail);
        if (!row.title.empty())
            CHECK(ev->title == row.title);
        for (const char* tag : row.offers)
            CHECK(has_recovery(*ev, tag));
        for (const char* tag : row.withholds)
            CHECK_FALSE(has_recovery(*ev, tag));
        if (*row.first) {
            REQUIRE_FALSE(ev->recovery_actions.empty());
            CHECK(ev->recovery_actions.front().log_tag == row.first);
            CHECK(ev->recovery_actions.front().style == "primary");
            for (const auto& a : ev->recovery_actions)
                if (a.log_tag != row.first && a.log_tag == "hh::recover")
                    CHECK(a.style.empty());
        }
    }
}

TEST_CASE("Happy Hare classify_error: a runout pause is a CRITICAL fault resumed first",
          "[ams][happy_hare][error-center]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    hh.initialize_test_gates(4);
    hh.test_parse_mmu_state({{"action", "Idle"},
                             {"print_state", "pause_locked"},
                             {"filament_pos", 8},
                             {"reason_for_pause", "Runout detected on gate 0  EndlessSpool mode is "
                                                  "off - manual intervention is required"}});
    helix::ClassifyContext ctx;
    ctx.is_paused = true;

    const auto ev = hh.classify_error("!! Runout detected", ctx);
    REQUIRE(ev.has_value());
    // The runout fault the hook raises is what the trait claims.
    CHECK(hh.traits().raises_runout_fault);
    CHECK(hh.traits().raises_runout_fault_during_job);
    CHECK(ev->source == helix::ErrorSource::HAPPY_HARE);
    CHECK(ev->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE_FALSE(ev->recovery_actions.empty());
    CHECK(ev->recovery_actions.front().gcode == "RESUME");
    // The detail is the descriptive reason, not the bare !! line.
    CHECK(ev->detail.find("Runout detected on gate 0") != std::string::npos);
}

TEST_CASE("Happy Hare pause_locked marks the slot and path, and cancel clears them",
          "[ams][happy_hare][error-center][1323]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    hh.initialize_test_gates(4);

    nlohmann::json fault;
    fault["action"] = "Idle";
    fault["gate"] = 2;
    fault["filament_pos"] = 3;
    fault["print_state"] = "pause_locked";
    fault["reason_for_pause"] = "Gate 2 is empty!";
    hh.test_parse_mmu_state(fault);

    auto slot = hh.get_slot_info(2);
    REQUIRE(slot.error.has_value());
    CHECK(slot.error->message == "Gate 2 is empty!");
    CHECK(hh.infer_error_segment() == path_segment_from_happy_hare_pos(3));

    // MMU_UNLOCK: still paused, HH keeps publishing the same reason (unchanged, so
    // a delta frame omits it).
    nlohmann::json unlocked;
    unlocked["print_state"] = "paused";
    hh.test_parse_mmu_state(unlocked);
    CHECK(hh.get_slot_info(2).error.has_value());

    // Cancel leaves the paused states; the published reason goes empty.
    nlohmann::json cancelled;
    cancelled["print_state"] = "cancelled";
    cancelled["reason_for_pause"] = "";
    hh.test_parse_mmu_state(cancelled);
    CHECK_FALSE(hh.get_slot_info(2).error.has_value());
    CHECK(hh.infer_error_segment() == PathSegment::NONE);
}

TEST_CASE("Happy Hare claims its own error notice prompt as a duplicate of the fault",
          "[ams][happy_hare][error-center][1323]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    CHECK(hh.duplicates_firmware_prompt("Happy Hare Error Notice"));
    CHECK_FALSE(hh.duplicates_firmware_prompt("Filament Change"));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Happy Hare's own error notice closes when the HH recovery popup shows",
                 "[ams][happy_hare][error-center][1323]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    hh.initialize_test_gates(4);

    helix::ActionPromptManager prompts;
    helix::ActionPromptManager::set_instance(&prompts);
    // _MMU_ERROR_DIALOG runs ahead of the `!!` line, so its prompt is up first.
    prompts.process_line("// action:prompt_begin Happy Hare Error Notice");
    prompts.process_line("// action:prompt_text Reason: Gate 2 is empty!");
    prompts.process_line("// action:prompt_show");
    REQUIRE(helix::ActionPromptManager::is_showing());

    helix::ClassifyContext ctx;
    auto ev = hh.classify_error("!! MMU issue: Gate 2 is empty!", ctx);
    REQUIRE(ev.has_value());
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    presenter.present(*ev);
    process_lvgl(20);

    CHECK(presenter.is_visible());
    CHECK_FALSE(helix::ActionPromptManager::is_showing());

    presenter.dismiss();
    process_lvgl(20);
    helix::ActionPromptManager::set_instance(nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "An unrelated firmware prompt survives the HH recovery popup",
                 "[ams][happy_hare][error-center][1323]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    hh.initialize_test_gates(4);

    helix::ActionPromptManager prompts;
    helix::ActionPromptManager::set_instance(&prompts);
    prompts.process_line("// action:prompt_begin Filament Change");
    prompts.process_line("// action:prompt_show");

    helix::ClassifyContext ctx;
    auto ev = hh.classify_error("!! MMU issue: Gate 2 is empty!", ctx);
    REQUIRE(ev.has_value());
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    presenter.present(*ev);
    process_lvgl(20);

    CHECK(helix::ActionPromptManager::is_showing());

    presenter.dismiss();
    process_lvgl(20);
    helix::ActionPromptManager::set_instance(nullptr);
}

// ============================================================================
// Connect-time configfile query
// ============================================================================

namespace {

/// Records every printer.objects.query and holds its success callback so the
/// test decides when, and with what, the printer "answers".
class QueryCapturingClient : public MoonrakerClientMock {
  public:
    QueryCapturingClient() : MoonrakerClientMock(MoonrakerClientMock::PrinterType::VORON_24) {}

    using MoonrakerClientMock::send_jsonrpc;
    helix::RequestId send_jsonrpc(
        const std::string& method, const nlohmann::json& params,
        helix::SharedCallback<void(const nlohmann::json&)> success_cb,
        helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        if (method == "printer.objects.query") {
            queries.push_back(params);
            answer = std::move(success_cb);
            return 0;
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

    std::vector<nlohmann::json> queries;
    helix::SharedCallback<void(const nlohmann::json&)> answer;
};

} // namespace

TEST_CASE("Happy Hare connect asks configfile once and applies it to every reader",
          "[ams][happy_hare][config]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    HappyHareTestAccess::on_started(helper);

    REQUIRE(client.queries.size() == 1);
    REQUIRE(client.answer);

    client.answer(
        nlohmann::json{{"result",
                        {{"status",
                          {{"configfile",
                            {{"settings",
                              {{"mmu",
                                {{"form_tip_macro", "_MMU_CUT_TIP"},
                                 {"gear_from_buffer_speed", "175"},
                                 {"heater_max_temp", 65.0}}},
                               {"mmu_machine",
                                {{"selector_type", "VirtualSelector"},
                                 {"filament_heater", "heater_generic box1_heater"}}}}}}}}}}}});
    helix::ui::UpdateQueue::instance().drain();

    CHECK(helper.get_system_info().tip_method == TipMethod::CUT);
    CHECK(helper.get_topology() == PathTopology::HUB);
    CHECK(helper.get_dryer_info().supported);
    CHECK(helper.get_dryer_info().max_temp_c == Catch::Approx(65.0f));
    CHECK(HappyHareTestAccess::config_defaults(helper).gear_from_buffer_speed ==
          Catch::Approx(175.0f));
    CHECK(HappyHareTestAccess::config_defaults(helper).loaded);
}

// ============================================================================
// Golden sequence: one full printer.mmu frame, then the deltas a Happy Hare box
// sends for a load, an unload, a pause and its resume, a gate insert, hex-only
// gate colours, per-gate drying and a topology change. Each frame is pinned as
// a text snapshot of everything the parse leaves behind plus the effects it
// dispatches.
// ============================================================================

namespace {

std::string join_ints(const std::vector<int>& values) {
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

nlohmann::json golden_hh_full_frame() {
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

} // namespace

namespace {
const std::string kHappyHareGolden = R"GOLD(full frame
slot=1 tool=1 loaded=true action=Idle detail='Idle' bypass=true units=1 hub=true slot_sensors=true
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
slot=1 tool=1 loaded=true action=Loading detail='Loading' bypass=true units=1 hub=true slot_sensors=true
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
slot=2 tool=2 loaded=true action=Idle detail='Idle' bypass=true units=1 hub=true slot_sensors=true
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
slot=2 tool=2 loaded=true action=Unloading detail='Unloading' bypass=true units=1 hub=true slot_sensors=true
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
slot=-1 tool=-1 loaded=false action=Idle detail='Idle' bypass=true units=1 hub=false slot_sensors=true
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
slot=1 tool=1 loaded=false action=Idle detail='Idle' bypass=true units=1 hub=true slot_sensors=true
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
slot=1 tool=1 loaded=false action=Idle detail='Idle' bypass=true units=1 hub=true slot_sensors=true
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
slot=1 tool=1 loaded=false action=Idle detail='Idle' bypass=true units=1 hub=true slot_sensors=true
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
slot=1 tool=1 loaded=false action=Idle detail='Idle' bypass=true units=1 hub=true slot_sensors=true
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
slot=1 tool=1 loaded=false action=Idle detail='Idle' bypass=true units=1 hub=true slot_sensors=true
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
slot=1 tool=1 loaded=false action=Idle detail='Idle' bypass=true units=1 hub=true slot_sensors=true
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
slot=1 tool=1 loaded=false action=Idle detail='Idle' bypass=true units=1 hub=true slot_sensors=true
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
slot=3 tool=1 loaded=false action=Idle detail='Idle' bypass=true units=1 hub=true slot_sensors=true
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
slot=3 tool=1 loaded=false action=Idle detail='Idle' bypass=true units=2 hub=true slot_sensors=true
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
slot=3 tool=1 loaded=false action=Idle detail='Idle' bypass=true units=2 hub=true slot_sensors=true
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
} // namespace

TEST_CASE_METHOD(HappyHareGoldenFixture, "Happy Hare golden status sequence",
                 "[ams][happy_hare][golden]") {
    using json = nlohmann::json;
    const std::vector<std::pair<std::string, json>> frames = {
        {"full frame", golden_hh_full_frame()},
        {"load: loading",
         json{{"action", "Loading"}, {"filament_pos", 3}, {"bowden_progress", 40}}},
        {"load: loaded", json{{"action", "Idle"},
                              {"filament_pos", 8},
                              {"bowden_progress", -1},
                              {"gate", 2},
                              {"tool", 2},
                              {"filament", "Loaded"}}},
        {"unload: unloading", json{{"action", "Unloading"}, {"filament_pos", 4}}},
        {"unload: unloaded", json{{"action", "Idle"},
                                  {"filament_pos", 0},
                                  {"filament", "Unloaded"},
                                  {"gate", -1},
                                  {"tool", -1}}},
        {"fault: paused", json{{"gate", 1},
                               {"tool", 1},
                               {"filament_pos", 5},
                               {"reason_for_pause", "Filament is stuck in the extruder"}}},
        {"fault: resumed", json{{"reason_for_pause", ""}}},
        {"insert: gate 3 fills", json{{"gate_status", json::array({1, 2, 0, 1})}}},
        {"insert: gate 2 fills", json{{"gate_status", json::array({1, 2, 1, 1})}}},
        {"hex colours only", json{{"gate_color", json::array({"ff8800", "", "#00ff00", "zzz"})}}},
        {"temperature + name deltas",
         json{{"gate_temperature", json::array({215})}, {"gate_name", json::array({"Crimson"})}}},
        {"per-gate drying array",
         json{{"drying_state", json::array({"active", "", "queued", ""})}}},
        {"aggregate sensors", json{{"gate", 3}, {"sensors", json{{"mmu_pre_gate", true}}}}},
        {"topology string", json{{"num_gates", "6,4"}, {"num_units", 2}, {"unit", 1}}},
        {"idle sparse frame", json{{"filament_pos", 7}}},
    };

    std::string all;
    for (const auto& [name, frame] : frames) {
        all += name + "\n" + feed(frame) + "\n";
    }
    CHECK(all == kHappyHareGolden);
}

// ============================================================================
// Happy Hare v4
// ============================================================================

namespace {

/// Answer the connect-time query with a golden fixture's configfile and live
/// mmu_machine, the way a v4 printer does.
void connect_with_fixture(AmsBackendHappyHareTestHelper& helper, QueryCapturingClient& client,
                          const nlohmann::json& fx) {
    HappyHareTestAccess::on_started(helper);
    REQUIRE(client.answer);
    client.answer(nlohmann::json{
        {"result",
         {{"status",
           {{"configfile", {{"settings", fx["configfile_settings"]}}},
            {"mmu_machine",
             fx.contains("mmu_machine") ? fx["mmu_machine"] : nlohmann::json::object()}}}}}});
    helix::ui::UpdateQueue::instance().drain();
}

/// One status notification carrying @p params as the printer sends them:
/// printer.mmu beside whatever sibling objects changed.
void feed_params(AmsBackendHappyHareTestHelper& helper, const nlohmann::json& params) {
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    HappyHareTestAccess::handle_status_update(helper, notification);
}

} // namespace

TEST_CASE("Happy Hare v4 reads its tunables from the split config sections",
          "[ams][happy_hare][hh_v4]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    connect_with_fixture(helper, client,
                         helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json"));

    const auto info = helper.get_system_info();
    CHECK(info.version == "4.0.0");
    CHECK(info.tip_method == TipMethod::CUT);
    CHECK(helper.get_dryer_info().max_temp_c == Catch::Approx(65.0f));
    const auto& d = HappyHareTestAccess::config_defaults(helper);
    CHECK(d.loaded);
    CHECK(d.gear_from_spool_speed == Catch::Approx(80.0f));
    CHECK(d.gear_from_buffer_speed == Catch::Approx(150.0f));
    CHECK(d.gear_unload_speed == Catch::Approx(120.0f));
    CHECK(d.extruder_load_speed == Catch::Approx(12.0f));
    CHECK(d.extruder_unload_speed == Catch::Approx(12.0f));
    CHECK(d.toolhead_extruder_to_nozzle == Catch::Approx(87.0f));
    CHECK(d.toolhead_sensor_to_nozzle == Catch::Approx(1.0f));
    CHECK(d.toolhead_entry_to_extruder == Catch::Approx(6.0f));
    CHECK(d.sync_to_extruder == 1);
}

TEST_CASE("Happy Hare v4 multi-unit reads unit parameters from the first unit's section",
          "[ams][happy_hare][hh_v4]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(10);

    connect_with_fixture(helper, client,
                         helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json"));

    CHECK(helper.get_system_info().tip_method == TipMethod::TIP_FORM);
    const auto& d = HappyHareTestAccess::config_defaults(helper);
    CHECK(d.gear_from_spool_speed == Catch::Approx(90.0f));
    CHECK(d.selector_move_speed == Catch::Approx(220.0f));
    CHECK(d.extruder_unload_speed == Catch::Approx(20.0f));
    CHECK(d.toolhead_ooze_reduction == Catch::Approx(1.5f));
}

TEST_CASE("Happy Hare v4 takes bypass support from the units, not printer.mmu",
          "[ams][happy_hare][hh_v4]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;

    SECTION("no unit has a bypass: status first, then config") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        CHECK_FALSE(helper.get_system_info().supports_bypass);
        // printer.mmu.has_bypass stays a constant true on later frames.
        helper.test_parse_mmu_state({{"has_bypass", true}});
        CHECK_FALSE(helper.get_system_info().supports_bypass);
    }
    SECTION("no unit has a bypass: config first, then status") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
        connect_with_fixture(helper, client, fx);
        helper.test_parse_mmu_state(fx["mmu_status"]);
        CHECK_FALSE(helper.get_system_info().supports_bypass);
    }
    SECTION("one unit of two has a bypass") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
        connect_with_fixture(helper, client, fx);
        helper.test_parse_mmu_state(fx["mmu_status"]);
        CHECK(helper.get_system_info().supports_bypass);
    }
}

TEST_CASE("Happy Hare v3 still takes bypass support from printer.mmu.has_bypass",
          "[ams][happy_hare][hh_v4]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    // v3.4 mmu_machine: per-unit objects, a has_bypass of its own, no version.
    const nlohmann::json fx = {
        {"configfile_settings", {{"mmu", {{"form_tip_macro", "_MMU_FORM_TIP"}}}}},
        {"mmu_machine", {{"unit_0", {{"name", "ERCF"}, {"has_bypass", false}}}, {"num_units", 1}}}};
    connect_with_fixture(helper, client, fx);
    helper.test_parse_mmu_state({{"gate_status", {1, 1}}, {"has_bypass", true}});
    CHECK(helper.get_system_info().supports_bypass);
    helper.test_parse_mmu_state({{"has_bypass", false}});
    CHECK_FALSE(helper.get_system_info().supports_bypass);
}

TEST_CASE("Happy Hare v4 reads per-gate entry sensors from their Klipper objects",
          "[ams][happy_hare][hh_v4]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");

    // The first frame carries printer.mmu (gate 7 selected, so its sensors dict
    // holds only generic names) beside the four entry sensor objects of unit 1.
    nlohmann::json params = fx["entry_sensors"];
    params["mmu"] = fx["mmu_status"];
    feed_params(helper, params);

    for (int gate = 0; gate < 6; ++gate) {
        INFO("gate " << gate);
        CHECK_FALSE(helper.slot_has_prep_sensor(gate));
    }
    CHECK(helper.slot_has_prep_sensor(6));
    CHECK(helper.get_gate_sensor(6)->pre_gate_triggered);
    CHECK(helper.get_gate_sensor(7)->pre_gate_triggered);
    CHECK_FALSE(helper.get_gate_sensor(8)->pre_gate_triggered);
    CHECK(helper.get_gate_sensor(9)->pre_gate_triggered);
    CHECK(helper.get_slot_filament_segment(9) == PathSegment::PREP);

    // A later sensors dict naming only the selected gate leaves the rest alone.
    helper.test_parse_mmu_state({{"sensors", {{"mmu_entry", false}, {"mmu_pre_gate", false}}}});
    CHECK(helper.get_gate_sensor(6)->pre_gate_triggered);
    CHECK(helper.get_gate_sensor(9)->pre_gate_triggered);

    // Deltas: one field at a time.
    feed_params(helper, {{"filament_switch_sensor mmu_entry_9", {{"filament_detected", false}}}});
    CHECK_FALSE(helper.get_gate_sensor(9)->pre_gate_triggered);
    feed_params(helper, {{"filament_switch_sensor mmu_entry_8", {{"filament_detected", true}}}});
    CHECK(helper.get_gate_sensor(8)->pre_gate_triggered);
}

TEST_CASE("Happy Hare v4 entry sensor that is disabled reads as not triggered",
          "[ams][happy_hare][hh_v4]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
    nlohmann::json params = fx["entry_sensors"];
    params["mmu"] = fx["mmu_status"];
    feed_params(helper, params);
    int state_events = 0;
    helper.set_event_callback([&](const std::string& name, const std::string&) {
        state_events += name == helix::AmsBackend::EVENT_STATE_CHANGED ? 1 : 0;
    });

    REQUIRE(helper.slot_has_prep_sensor(3));
    CHECK_FALSE(helper.get_gate_sensor(3)->pre_gate_triggered); // detected, but disabled
    CHECK(helper.get_gate_sensor(0)->pre_gate_triggered);
    CHECK_FALSE(helper.get_gate_sensor(2)->pre_gate_triggered);
    feed_params(helper, {{"filament_switch_sensor mmu_entry_3", {{"enabled", true}}}});
    CHECK(helper.get_gate_sensor(3)->pre_gate_triggered);
    CHECK(state_events == 1); // a sensor-only frame still reaches the UI
    CHECK(helper.get_system_info().units[0].has_slot_sensors);
}

TEST_CASE("Happy Hare v4 sensors dict with no gate selected names gates mmu_entry_N",
          "[ams][happy_hare][hh_v4]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.test_parse_mmu_state({{"gate_status", {1, 1, 0, 1}},
                                 {"gate", -1},
                                 {"sensors",
                                  {{"mmu_entry_0", true},
                                   {"mmu_entry_1", false},
                                   {"mmu_entry_2", nullptr},
                                   {"mmu_shared_exit", false}}}});
    REQUIRE(helper.get_gate_sensor(0));
    CHECK(helper.get_gate_sensor(0)->pre_gate_triggered);
    CHECK_FALSE(helper.get_gate_sensor(1)->pre_gate_triggered);
    REQUIRE(helper.get_gate_sensor(2));
    CHECK_FALSE(helper.get_gate_sensor(2)->pre_gate_triggered);
    CHECK_FALSE(helper.get_gate_sensor(3));
}

TEST_CASE("Happy Hare MMU_TEST_CONFIG names gear speeds the way each version does",
          "[ams][happy_hare][hh_v4]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    SECTION("v3") {
        helper.set_config_defaults_for_test();
        helper.execute_device_action("gear_from_spool_speed", std::any(70.0));
        helper.execute_device_action("gear_from_buffer_speed", std::any(160.0));
        helper.execute_device_action("toolhead_ooze_reduction", std::any(2.5));
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_TEST_CONFIG GEAR_FROM_SPOOL_SPEED=70",
                                       "MMU_TEST_CONFIG GEAR_FROM_BUFFER_SPEED=160",
                                       "MMU_TEST_CONFIG TOOLHEAD_OOZE_REDUCTION=2.5"});
    }
    SECTION("v4") {
        connect_with_fixture(
            helper, client, helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json"));
        helper.clear_captured_gcodes();
        helper.execute_device_action("gear_from_spool_speed", std::any(70.0));
        // The QIDI unit has no filament buffer: v4 would refuse the parameter.
        CHECK_FALSE(helper.execute_device_action("gear_from_buffer_speed", std::any(160.0)));
        helper.execute_device_action("toolhead_ooze_reduction", std::any(2.5));
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_TEST_CONFIG GEAR_LOAD_SPEED=70",
                                       "MMU_TEST_CONFIG TOOLHEAD_OOZE_REDUCTION=2.5"});
    }
}

TEST_CASE("Happy Hare v4 reapplies persisted overrides under the v4 names",
          "[ams][happy_hare][hh_v4]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    connect_with_fixture(helper, client,
                         helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json"));
    auto& overrides = HappyHareTestAccess::user_overrides(helper);
    overrides = {};
    overrides.gear_from_spool_speed = 75.0f;
    overrides.extruder_load_speed = 20.0f;
    // The QIDI unit is always gripped, has no buffer and no encoder: v4 refuses
    // all three, so they are left out rather than sinking the rest.
    overrides.sync_to_extruder = 0;
    overrides.gear_from_buffer_speed = 160.0f;
    overrides.clog_detection = 2;
    helper.clear_captured_gcodes();

    helper.test_reapply_overrides();

    CHECK(helper.captured_gcodes ==
          std::vector<std::string>{"MMU_TEST_CONFIG GEAR_LOAD_SPEED=75",
                                   "MMU_TEST_CONFIG EXTRUDER_LOAD_SPEED=20"});
}

namespace {

/// Every command whose shape depends on the version or the unit count, in a
/// fixed order, with the selected unit already set by the caller.
std::vector<std::string> unit_scoped_commands(AmsBackendHappyHareTestHelper& helper,
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

} // namespace

TEST_CASE("Happy Hare names the unit only where v4 multi-unit requires it",
          "[ams][happy_hare][hh_v4]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;

    SECTION("v3 single unit") {
        helper.initialize_test_gates(4);
        helper.set_config_defaults_for_test();
        CHECK(unit_scoped_commands(helper, 0) ==
              std::vector<std::string>{"MMU_HOME", "MMU_MOTORS_ON", "MMU_MOTORS_OFF",
                                       "MMU_HEATER DRY=1 TEMP=50 TIMER=60", "MMU_HEATER TEMP=55",
                                       "MMU_HEATER STOP=1", "MMU_SERVO POS=up",
                                       "MMU_CALIBRATE_GATES", "MMU_CALIBRATE_BOWDEN",
                                       "MMU_TEST_GRIP", "MMU_TEST_CONFIG GEAR_FROM_SPOOL_SPEED=70",
                                       "MMU_TEST_CONFIG EXTRUDER_LOAD_SPEED=20",
                                       "MMU_TEST_CONFIG TOOLHEAD_OOZE_REDUCTION=1.0",
                                       "MMU_TEST_CONFIG SYNC_TO_EXTRUDER=1"});
    }
    SECTION("v4 single unit") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        CHECK(unit_scoped_commands(helper, 0) ==
              std::vector<std::string>{
                  "MMU_HOME", "MMU_MOTORS_ON", "MMU_MOTORS_OFF",
                  "MMU_HEATER DRY=1 TEMP=50 TIMER=60", "MMU_HEATER TEMP=55", "MMU_HEATER STOP=1",
                  // A VirtualSelector, encoder-less, always-gripped
                  // unit: no servo, gate calibration or sync.
                  "MMU_CALIBRATE_BOWDEN", "MMU_TEST_GRIP", "MMU_TEST_CONFIG GEAR_LOAD_SPEED=70",
                  "MMU_TEST_CONFIG EXTRUDER_LOAD_SPEED=20",
                  "MMU_TEST_CONFIG TOOLHEAD_OOZE_REDUCTION=1.0"});
    }
    SECTION("v4 two units, unit 1 selected") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        CHECK(unit_scoped_commands(helper, 1) ==
              std::vector<std::string>{"MMU_HOME UNIT=ALL", "MMU_MOTORS_ON UNIT=ALL",
                                       "MMU_MOTORS_OFF UNIT=ALL",
                                       "MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=6,7,8,9",
                                       "MMU_HEATER TEMP=55 UNIT=1 GATES=6,7,8,9",
                                       "MMU_HEATER STOP=1 UNIT=1 GATES=6,7,8,9",
                                       // The selected Box Turtle has no servo, encoder or ungripped
                                       // drive, so those go to the ERCF.
                                       "MMU_SERVO POS=up UNIT=0", "MMU_CALIBRATE_GATE ALL=1 UNIT=0",
                                       "MMU_CALIBRATE_BOWDEN", "MMU_TEST_GRIP UNIT=1",
                                       "MMU_TEST_CONFIG GEAR_LOAD_SPEED=70 UNIT=1",
                                       "MMU_TEST_CONFIG EXTRUDER_LOAD_SPEED=20",
                                       "MMU_TEST_CONFIG TOOLHEAD_OOZE_REDUCTION=1.0 UNIT=1",
                                       "MMU_TEST_CONFIG SYNC_TO_EXTRUDER=1 UNIT=0"});

        auto& overrides = HappyHareTestAccess::user_overrides(helper);
        overrides = {};
        overrides.gear_from_spool_speed = 75.0f;
        overrides.gear_from_buffer_speed = 160.0f;
        overrides.selector_move_speed = 180.0f;
        overrides.extruder_unload_speed = 25.0f;
        helper.clear_captured_gcodes();
        helper.test_reapply_overrides();
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_TEST_CONFIG GEAR_FROM_FILAMENT_BUFFER_SPEED=160 UNIT=0",
                                       "MMU_TEST_CONFIG GEAR_LOAD_SPEED=75 UNIT=1",
                                       "MMU_TEST_CONFIG SELECTOR_MOVE_SPEED=180 UNIT=0",
                                       "MMU_TEST_CONFIG EXTRUDER_UNLOAD_SPEED=25"});

        helper.set_current_slot(-2);
        helper.clear_captured_gcodes();
        CHECK(helper.disable_bypass().success());
        CHECK(helper.captured_gcodes == std::vector<std::string>{"MMU_HOME UNIT=ALL"});
    }
}

TEST_CASE("Happy Hare v4 null telemetry clears the selected unit's readings",
          "[ams][happy_hare][hh_v4]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");

    helper.test_parse_mmu_state(fx["mmu_status"]);
    auto info = helper.get_system_info();
    CHECK_FALSE(helix::buffer_reading(info, -1).present());
    CHECK(info.sync_feedback_bias == Catch::Approx(-2.0f));
    CHECK(info.sync_feedback_flow_rate == Catch::Approx(-1.0f));
    CHECK(info.sync_feedback_state.empty());

    // A unit with a buffer and an encoder is selected...
    helper.test_parse_mmu_state(
        {{"sync_feedback_bias_modelled", 0.25},
         {"sync_feedback_bias_raw", 0.5},
         {"sync_feedback_state", "neutral"},
         {"encoder", {{"flow_rate", 95}}},
         {"flowguard", {{"enabled", true}, {"level", 0.4}, {"encoder_mode", 2}}}});
    info = helper.get_system_info();
    REQUIRE(info.sync_feedback_bias == Catch::Approx(0.25f));
    REQUIRE(helix::buffer_reading(info, -1).present());
    REQUIRE(info.encoder_info.enabled);
    REQUIRE(info.flowguard_info.enabled);

    // ...then one with neither: its nulls are "no reading", not the last unit's.
    helper.test_parse_mmu_state({{"sync_feedback_bias_modelled", nullptr},
                                 {"sync_feedback_bias_raw", nullptr},
                                 {"encoder", nullptr},
                                 {"flowguard", nullptr},
                                 {"tangle_prevention", nullptr}});
    info = helper.get_system_info();
    CHECK(info.sync_feedback_bias == Catch::Approx(-2.0f));
    CHECK(info.sync_feedback_bias_raw == Catch::Approx(-2.0f));
    CHECK_FALSE(helix::buffer_reading(info, -1).present());
    CHECK_FALSE(info.encoder_info.enabled);
    CHECK_FALSE(info.flowguard_info.enabled);
    // Fields nothing reset keep their value.
    CHECK(info.encoder_info.flow_rate == 95);
    CHECK(info.flowguard_info.level == Catch::Approx(0.4f));
}

TEST_CASE("Happy Hare v4 shows the selected gate's eSpooler operation",
          "[ams][happy_hare][hh_v4]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");

    // gate 7, list says "assist", deprecated espooler_active says "".
    helper.test_parse_mmu_state(fx["mmu_status"]);
    CHECK(helper.get_system_info().espooler_state == "assist");

    // The gate moves without the list being resent.
    helper.test_parse_mmu_state({{"gate", 9}});
    CHECK(helper.get_system_info().espooler_state == "rewind");
    CHECK(HappyHareTestAccess::espooler_active(helper) == "rewind");

    // The deprecated alias no longer speaks once the list has.
    helper.test_parse_mmu_state({{"espooler_active", "print"}});
    CHECK(helper.get_system_info().espooler_state == "rewind");

    helper.test_parse_mmu_state({{"gate", -1}});
    CHECK(helper.get_system_info().espooler_state.empty());
}

// ============================================================================
// Multi-unit from mmu_machine
// ============================================================================

TEST_CASE("Happy Hare takes each unit's split, name and topology from mmu_machine",
          "[ams][happy_hare][hh_multi_unit]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");

    auto check_units = [&] {
        const auto info = helper.get_system_info();
        REQUIRE(info.units.size() == 2);
        CHECK(info.units[0].name == "ERCF");
        CHECK(info.units[0].first_slot_global_index == 0);
        CHECK(info.units[0].slot_count == 6);
        CHECK(info.units[0].topology == PathTopology::LINEAR);
        CHECK(info.units[0].has_encoder);
        CHECK(info.units[1].name == "Box Turtle");
        CHECK(info.units[1].first_slot_global_index == 6);
        CHECK(info.units[1].slot_count == 4);
        CHECK(info.units[1].topology == PathTopology::HUB);
        CHECK_FALSE(info.units[1].has_encoder);
        CHECK(helper.get_unit_topology(1) == PathTopology::HUB);
        CHECK(info.total_slots == 10);
    };

    SECTION("status first: the re-split keeps every gate's state") {
        helper.test_parse_mmu_state(fx["mmu_status"]);
        helper.test_parse_mmu_state(
            {{"gate_color", {"FF0000", "", "", "", "", "", "", "", "", "00FF00"}}});
        REQUIRE(helper.get_system_info().units.size() == 1);
        connect_with_fixture(helper, client, fx);
        check_units();
        CHECK(helper.get_slot_info(0).color_rgb == 0xFF0000u);
        CHECK(helper.get_slot_info(9).color_rgb == 0x00FF00u);
        CHECK(helper.get_slot_info(3).status == SlotStatus::EMPTY);
    }
    SECTION("config first") {
        connect_with_fixture(helper, client, fx);
        helper.test_parse_mmu_state(fx["mmu_status"]);
        check_units();
    }
}

TEST_CASE("Happy Hare single v4 unit is named by its display_name",
          "[ams][happy_hare][hh_multi_unit]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
    connect_with_fixture(helper, client, fx);
    helper.test_parse_mmu_state(fx["mmu_status"]);
    const auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    CHECK(info.units[0].name == "QIDI-0");
    CHECK(info.units[0].topology == PathTopology::HUB);
}

TEST_CASE("Happy Hare reads every unit's enclosure heaters, not only unit 0's",
          "[ams][happy_hare][hh_multi_unit]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    connect_with_fixture(helper, client, fx);
    helper.test_parse_mmu_state(fx["mmu_status"]);

    CHECK(helper.get_dryer_info().supported);
    const auto& heaters = HappyHareTestAccess::filament_heaters(helper);
    REQUIRE(heaters.size() == 10);
    CHECK(heaters[0].empty());
    CHECK(heaters[6] == "heater_generic bt_heat0");
    CHECK(heaters[9] == "heater_generic bt_heat1");

    const auto zones = helper.get_environment_zones(-1);
    REQUIRE(zones.size() == 2);
    CHECK(zones[0].gates == std::vector<int>{6, 7});
    CHECK(zones[0].unit_index == 1);
    CHECK(zones[1].gates == std::vector<int>{8, 9});
    CHECK(helper.get_environment_zones(0).empty());
}

TEST_CASE("Happy Hare v4 reads clog detection mode from the encoder mode",
          "[ams][happy_hare][hh_v4][clog]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.test_parse_mmu_state({{"gate_status", {1, 1}}, {"clog_detection_enabled", false}});
    CHECK(helper.get_system_info().encoder_info.detection_mode == 0);

    // A unit with a buffer and an encoder: buffer FlowGuard plus the mode.
    helper.test_parse_mmu_state(
        {{"clog_detection_enabled", false},
         {"flowguard", {{"enabled", true}, {"level", 0.1}, {"trigger", ""}, {"encoder_mode", 1}}}});
    auto info = helper.get_system_info();
    CHECK(info.clog_detection == 1);
    CHECK(info.encoder_info.detection_mode == 1);
    CHECK(info.encoder_info.enabled);
    CHECK(info.flowguard_info.enabled);

    // An encoder-only unit publishes flowguard as {active, enabled,
    // encoder_mode}: the mode, but no buffer FlowGuard to meter.
    helper.test_parse_mmu_state(
        {{"flowguard", {{"active", false}, {"enabled", 1}, {"encoder_mode", 2}}},
         {"encoder", {{"detection_mode", 1}, {"flow_rate", 100}}}});
    info = helper.get_system_info();
    CHECK(info.encoder_info.detection_mode == 2); // configured, not the runtime state
    CHECK(info.encoder_info.enabled);
    CHECK_FALSE(info.flowguard_info.enabled);
    // A runtime encoder state on its own is not the configured mode.
    helper.test_parse_mmu_state({{"encoder", {{"detection_mode", 0}}}});
    CHECK(helper.get_system_info().encoder_info.detection_mode == 2);

    // v3's integer wins whenever it is published.
    helper.test_parse_mmu_state(
        {{"clog_detection_enabled", 2}, {"flowguard", {{"encoder_mode", 1}}}});
    CHECK(helper.get_system_info().encoder_info.detection_mode == 2);
}

TEST_CASE("Happy Hare v4 writes clog detection mode as the selected unit's encoder mode",
          "[ams][happy_hare][hh_v4][clog]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    fx["mmu_status"]["unit"] = 0;
    fx["mmu_status"]["gate"] = 2;
    helper.test_parse_mmu_state(fx["mmu_status"]);
    connect_with_fixture(helper, client, fx);

    CHECK(helper.clog_detection_mode_gcode(1, 12.0f) ==
          std::optional<std::string>(
              "MMU_TEST_CONFIG flowguard_encoder_mode=1 flowguard_encoder_max_motion=12.0 UNIT=0"));
    CHECK(helper.clog_detection_mode_gcode(2, 12.0f) ==
          std::optional<std::string>("MMU_TEST_CONFIG flowguard_encoder_mode=2 UNIT=0"));
    helper.clear_captured_gcodes();
    helper.execute_device_action("clog_detection", std::string("Auto"));
    CHECK(helper.captured_gcodes ==
          std::vector<std::string>{"MMU_TEST_CONFIG FLOWGUARD_ENCODER_MODE=2 UNIT=0"});

    // With the encoder-less Box Turtle selected, the mode goes to the ERCF.
    helper.test_parse_mmu_state({{"unit", 1}, {"gate", 7}});
    CHECK(helper.clog_detection_mode_gcode(2, 0.0f) ==
          std::optional<std::string>("MMU_TEST_CONFIG flowguard_encoder_mode=2 UNIT=0"));
}

TEST_CASE("Happy Hare clog detection mode command on v3 and a single v4 unit",
          "[ams][happy_hare][hh_v4][clog]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.set_config_defaults_for_test();

    SECTION("v3") {
        CHECK(helper.clog_detection_mode_gcode(1, 12.0f) ==
              std::optional<std::string>(
                  "MMU_TEST_CONFIG enable_clog_detection=1 mmu_calibration_clog_length=12.0"));
        helper.execute_device_action("clog_detection", std::string("Manual"));
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_TEST_CONFIG ENABLE_CLOG_DETECTION=1"});
    }
    SECTION("v4 single unit with an encoder") {
        auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
        fx["mmu_machine"]["unit_0"]["selector_type"] = "LinearSelector";
        fx["configfile_settings"]["mmu_unit unit0"]["encoder"] = "unit0_encoder";
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        helper.clear_captured_gcodes();
        CHECK(helper.clog_detection_mode_gcode(1, 12.0f) ==
              std::optional<std::string>(
                  "MMU_TEST_CONFIG flowguard_encoder_mode=1 flowguard_encoder_max_motion=12.0"));
        helper.execute_device_action("clog_detection", std::string("Off"));
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_TEST_CONFIG FLOWGUARD_ENCODER_MODE=0"});
    }
    SECTION("v4 single Type B unit has no encoder mode") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        CHECK_FALSE(helper.clog_detection_mode_gcode(2, 0.0f));
    }
}

// ============================================================================
// v4 per-unit guards
// ============================================================================

namespace {

bool action_enabled(const AmsBackendHappyHareTestHelper& helper, const std::string& id) {
    for (const auto& a : helper.get_device_actions()) {
        if (a.id == id) {
            return a.enabled;
        }
    }
    FAIL("no device action " << id);
    return false;
}

} // namespace

TEST_CASE("Happy Hare v4 disables the actions no unit's hardware takes",
          "[ams][happy_hare][hh_v4]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;

    SECTION("one QIDI unit: hub, no encoder, no buffer, always gripped, no toolhead sensor") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        for (const char* id : {"servo_up", "selector_speed", "calibrate_encoder", "calibrate_gates",
                               "clog_detection", "sync_to_extruder", "gear_from_buffer_speed",
                               "toolhead_sensor_to_nozzle"}) {
            INFO(id);
            CHECK_FALSE(action_enabled(helper, id));
        }
        for (const char* id : {"gear_from_spool_speed", "toolhead_entry_to_extruder",
                               "toolhead_ooze_reduction", "calibrate_bowden"}) {
            INFO(id);
            CHECK(action_enabled(helper, id));
        }
        helper.clear_captured_gcodes();
        CHECK_FALSE(helper.execute_device_action("toolhead_sensor_to_nozzle", std::any(30.0)));
        CHECK_FALSE(helper.execute_device_action("servo_up", {}));
        CHECK(helper.captured_gcodes.empty());
    }
    SECTION("ERCF beside a Box Turtle: every action has a unit to go to") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        for (const char* id : {"servo_up", "selector_speed", "calibrate_gates", "sync_to_extruder",
                               "gear_from_buffer_speed", "toolhead_sensor_to_nozzle"}) {
            INFO(id);
            CHECK(action_enabled(helper, id));
        }
    }
}

TEST_CASE("Happy Hare v4 whole-machine drying sends one MMU_HEATER per heated unit",
          "[ams][happy_hare][hh_v4]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    helper.test_parse_mmu_state(fx["mmu_status"]);

    SECTION("only the Box Turtle has heaters") {
        connect_with_fixture(helper, client, fx);
        helper.clear_captured_gcodes();
        CHECK(helper.stop_drying(-1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER STOP=1 UNIT=1 GATES=6,7,8,9"});
    }
    SECTION("both units heated") {
        fx["mmu_machine"]["unit_0"]["filament_heater"] = "heater_generic ercf_heater";
        connect_with_fixture(helper, client, fx);
        helper.clear_captured_gcodes();
        CHECK(helper.start_drying(50.0f, 60, -1, -1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=0 GATES=0,1,2,3,4,5",
                                       "MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=6,7,8,9"});
    }
}

TEST_CASE("Happy Hare re-split keeps sensors, the tool map and the hub state",
          "[ams][happy_hare][hh_multi_unit]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");

    nlohmann::json params = fx["entry_sensors"];
    params["mmu"] = fx["mmu_status"];
    params["mmu"]["ttg_map"] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    params["mmu"]["filament_pos"] = 5;
    feed_params(helper, params);
    REQUIRE(helper.get_system_info().units.size() == 1);
    REQUIRE(helper.get_system_info().units[0].hub_sensor_triggered);

    connect_with_fixture(helper, client, fx);
    const auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
    CHECK_FALSE(info.units[0].hub_sensor_triggered);
    CHECK(info.units[1].hub_sensor_triggered); // gate 7 is the Box Turtle's
    CHECK(info.units[1].has_slot_sensors);
    CHECK(helper.get_gate_sensor(7)->pre_gate_triggered);
    CHECK_FALSE(helper.get_gate_sensor(8)->pre_gate_triggered);
    CHECK(helper.get_tool_mapping() == std::vector<int>{9, 8, 7, 6, 5, 4, 3, 2, 1, 0});
    CHECK(helper.get_slot_info(0).mapped_tool == 9);
}

TEST_CASE("Happy Hare v3 connect keeps the hub state of a split it already has",
          "[ams][happy_hare][hh_multi_unit]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.test_parse_mmu_state({{"gate_status", {1, 1, 1, 1}}, {"gate", 1}, {"filament_pos", 5}});
    REQUIRE(helper.get_system_info().units[0].hub_sensor_triggered);
    connect_with_fixture(helper, client,
                         {{"configfile_settings",
                           {{"mmu", {{"happy_hare_version", 3.42}}},
                            {"mmu_machine", {{"selector_type", "LinearSelector"}}}}},
                          {"mmu_machine",
                           {{"unit_0", {{"name", "ERCF"}, {"num_gates", 4}, {"first_gate", 0}}},
                            {"num_units", 1}}}});
    const auto info = helper.get_system_info();
    CHECK(info.version == "3.42");
    REQUIRE(info.units.size() == 1);
    CHECK(info.units[0].hub_sensor_triggered);
}

namespace {

/// Snapshots the Config singleton's data and save path for one test, and
/// points saves at no file, so no write reaches disk and nothing a test sets
/// outlives it, even when a REQUIRE throws.
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

} // namespace

TEST_CASE("Happy Hare keeps an override saved against the built-in default",
          "[ams][happy_hare][hh_v4]") {
    ScopedConfig config;
    // Saved while the printer's own default could not be read: the record names
    // the built-in 60, the printer's real default is 80.
    config->set<float>("/hh_overrides/gear_from_spool_speed/value", 95.0f);
    config->set<float>("/hh_overrides/gear_from_spool_speed/config_default", 60.0f);
    // Saved against a real default the printer has since changed: stale.
    config->set<float>("/hh_overrides/gear_unload_speed/value", 99.0f);
    config->set<float>("/hh_overrides/gear_unload_speed/config_default", 111.0f);

    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    connect_with_fixture(helper, client,
                         helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json"));

    const auto& o = HappyHareTestAccess::user_overrides(helper);
    REQUIRE(o.gear_from_spool_speed);
    CHECK(*o.gear_from_spool_speed == Catch::Approx(95.0f));
    CHECK(config->get<float>("/hh_overrides/gear_from_spool_speed/config_default", 0.0f) ==
          Catch::Approx(80.0f));
    CHECK_FALSE(o.gear_unload_speed);
    CHECK(config->get_path().empty()); // the migration's save() wrote no file
}

TEST_CASE("Happy Hare selector speed override survives a restart under either key",
          "[ams][happy_hare][hh_v4]") {
    ScopedConfig config;
    SECTION("saved by the slider") {
        {
            helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
            AmsBackendHappyHareTestHelper& helper = *helper_reg;
            helper.initialize_test_gates(4);
            helper.set_config_defaults_for_test();
            REQUIRE(helper.execute_device_action("selector_speed", std::any(180.0)).success());
        }
        CHECK(config->exists("/hh_overrides/selector_move_speed/value"));
    }
    SECTION("an older record under the action id") {
        config->set<float>("/hh_overrides/selector_speed/value", 180.0f);
        config->set<float>("/hh_overrides/selector_speed/config_default", 200.0f);
    }
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    connect_with_fixture(helper, client,
                         {{"configfile_settings",
                           {{"mmu", {{"happy_hare_version", 3.42}, {"selector_move_speed", 200.0}}},
                            {"mmu_machine", {{"selector_type", "LinearSelector"}}}}}});
    const auto& o = HappyHareTestAccess::user_overrides(helper);
    REQUIRE(o.selector_move_speed);
    CHECK(*o.selector_move_speed == Catch::Approx(180.0f));
}

TEST_CASE("Happy Hare v3 eSpooler list covering only some gates is not read per gate",
          "[ams][happy_hare][hh_v4]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    // v3 lists only the gates with an eSpooler fitted: here two of four.
    helper.test_parse_mmu_state({{"gate_status", {1, 1, 1, 1}},
                                 {"gate", 1},
                                 {"espooler", {"rewind", "assist"}},
                                 {"espooler_active", "print"}});
    CHECK(helper.get_system_info().espooler_state == "print");
    // One entry per gate lines up even on v3.
    helper.test_parse_mmu_state({{"espooler", {"off", "rewind", "off", "off"}}});
    CHECK(helper.get_system_info().espooler_state == "rewind");
}

TEST_CASE("Happy Hare v4 shows no bypass before its units are known", "[ams][happy_hare][hh_v4]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    // The first v4 frame says has_bypass: true for every install.
    auto frame = fx["mmu_status"];
    frame["tangle_prevention"] = nullptr;
    helper.test_parse_mmu_state(frame);
    CHECK_FALSE(helper.get_system_info().supports_bypass);
    connect_with_fixture(helper, client, fx);
    CHECK(helper.get_system_info().supports_bypass); // the ERCF has one
}

// ============================================================================
// Version bands and round-3 guards
// ============================================================================

namespace {

nlohmann::json v3_fixture(double version, const std::string& selector = "LinearSelector") {
    return {{"configfile_settings",
             {{"mmu",
               {{"happy_hare_version", version},
                {"enable_clog_detection", 1},
                {"flowguard_encoder_mode", 1},
                {"flowguard_encoder_max_motion", 18.0}}},
              {"mmu_machine", {{"selector_type", selector}}}}}};
}

} // namespace

TEST_CASE("Happy Hare clog detection is named the way each version takes it",
          "[ams][happy_hare][hh_v4][clog]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);

    SECTION("before 3.42: ENABLE_CLOG_DETECTION and the calibrated clog length") {
        connect_with_fixture(helper, client, v3_fixture(3.40));
        helper.clear_captured_gcodes();
        CHECK(helper.clog_detection_mode_gcode(1, 12.0f) ==
              std::optional<std::string>(
                  "MMU_TEST_CONFIG enable_clog_detection=1 mmu_calibration_clog_length=12.0"));
        helper.execute_device_action("clog_detection", std::string("Auto"));
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_TEST_CONFIG ENABLE_CLOG_DETECTION=2"});
        CHECK_FALSE(helper.clog_detection_length_setting());
        CHECK(HappyHareTestAccess::config_defaults(helper).clog_detection == 1);
    }
    SECTION("3.42: the FlowGuard encoder parameters, no UNIT") {
        connect_with_fixture(helper, client, v3_fixture(3.42));
        helper.clear_captured_gcodes();
        CHECK(helper.clog_detection_mode_gcode(1, 12.0f) ==
              std::optional<std::string>(
                  "MMU_TEST_CONFIG flowguard_encoder_mode=1 flowguard_encoder_max_motion=12.0"));
        helper.execute_device_action("clog_detection", std::string("Off"));
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_TEST_CONFIG FLOWGUARD_ENCODER_MODE=0"});
        CHECK(helper.clog_detection_length_setting() == std::optional<float>(18.0f));
    }
    SECTION("v4 multi-unit: the encoder unit's own parameters") {
        auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        CHECK(helper.clog_detection_length_setting() == std::optional<float>(25.0f));
        CHECK(helper.clog_detection_mode_gcode(2, 0.0f) ==
              std::optional<std::string>("MMU_TEST_CONFIG flowguard_encoder_mode=2 UNIT=0"));
    }
}

TEST_CASE("Happy Hare reapplies each override alone, under its version's name",
          "[ams][happy_hare][hh_v4]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    auto set_overrides = [&] {
        auto& o = HappyHareTestAccess::user_overrides(helper);
        o = {};
        o.gear_from_spool_speed = 70.0f;
        o.gear_unload_speed = 90.0f;
        o.selector_move_speed = 180.0f;
        o.clog_detection = 2;
        helper.clear_captured_gcodes();
        helper.test_reapply_overrides();
        return helper.captured_gcodes;
    };

    SECTION("3.01: no gear unload speed yet") {
        connect_with_fixture(helper, client, v3_fixture(3.01));
        CHECK(set_overrides() ==
              std::vector<std::string>{"MMU_TEST_CONFIG GEAR_FROM_SPOOL_SPEED=70",
                                       "MMU_TEST_CONFIG SELECTOR_MOVE_SPEED=180",
                                       "MMU_TEST_CONFIG ENABLE_CLOG_DETECTION=2"});
    }
    SECTION("3.42 on a selector with no moving carriage") {
        connect_with_fixture(helper, client, v3_fixture(3.42, "VirtualSelector"));
        CHECK(set_overrides() ==
              std::vector<std::string>{"MMU_TEST_CONFIG GEAR_FROM_SPOOL_SPEED=70",
                                       "MMU_TEST_CONFIG GEAR_UNLOAD_SPEED=90",
                                       "MMU_TEST_CONFIG FLOWGUARD_ENCODER_MODE=2"});
    }
}

TEST_CASE("Happy Hare v4 frame names v4 parameters before the config query answers",
          "[ams][happy_hare][hh_v4]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.set_config_defaults_for_test();
    auto frame =
        helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json")["mmu_status"];
    frame["tangle_prevention"] = nullptr;
    helper.test_parse_mmu_state(frame);
    helper.clear_captured_gcodes();
    helper.execute_device_action("gear_from_spool_speed", std::any(70.0));
    CHECK(helper.captured_gcodes == std::vector<std::string>{"MMU_TEST_CONFIG GEAR_LOAD_SPEED=70"});
}

TEST_CASE("Happy Hare v4 treats a disabled toolhead sensor as not fitted",
          "[ams][happy_hare][hh_v4]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    helper.test_parse_mmu_state(fx["mmu_status"]);
    connect_with_fixture(helper, client, fx);
    helper.clear_captured_gcodes();
    REQUIRE(helper.execute_device_action("toolhead_sensor_to_nozzle", std::any(30.0)).success());
    // v4 publishes a disabled sensor as null, and refuses its distance.
    helper.test_parse_mmu_state({{"sensors", {{"toolhead", nullptr}, {"extruder", false}}}});
    CHECK_FALSE(helper.execute_device_action("toolhead_sensor_to_nozzle", std::any(31.0)));
    CHECK(helper.captured_gcodes ==
          std::vector<std::string>{"MMU_TEST_CONFIG TOOLHEAD_SENSOR_TO_NOZZLE=30.0 UNIT=1"});
}

TEST_CASE("Happy Hare v4 unit encoder flag comes from the unit's configured encoder",
          "[ams][happy_hare][hh_v4]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
    // A Type A unit configured without an encoder.
    fx["mmu_machine"]["unit_0"]["selector_type"] = "LinearSelector";
    helper.test_parse_mmu_state(fx["mmu_status"]);
    connect_with_fixture(helper, client, fx);
    const auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    CHECK(info.units[0].topology == PathTopology::LINEAR);
    CHECK_FALSE(info.units[0].has_encoder);
    CHECK_FALSE(action_enabled(helper, "calibrate_encoder"));
}

TEST_CASE("Happy Hare connect with an unchanged split does not rebuild the units",
          "[ams][happy_hare][hh_multi_unit]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.test_parse_mmu_state({{"gate_status", {1, 1, 1, 1}}, {"gate", 1}});
    helper.mark_unit(0, "kept");
    connect_with_fixture(helper, client, v3_fixture(3.42));
    CHECK(helper.unit_marker(0) == "kept");
}

TEST_CASE("Happy Hare whole-machine drying stops the units it started when one refuses",
          "[ams][happy_hare][hh_v4]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    fx["mmu_machine"]["unit_0"]["filament_heater"] = "heater_generic ercf_heater";
    helper.test_parse_mmu_state(fx["mmu_status"]);
    connect_with_fixture(helper, client, fx);
    helper.clear_captured_gcodes();
    helper.fail_gcode = "MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=6,7,8,9";

    CHECK_FALSE(helper.start_drying(50.0f, 60, -1, -1));
    CHECK(helper.captured_gcodes ==
          std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=0 GATES=0,1,2,3,4,5",
                                   "MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=6,7,8,9",
                                   "MMU_HEATER STOP=1 UNIT=0 GATES=0,1,2,3,4,5"});
}

TEST_CASE("Happy Hare v4 encoder flag holds when the split is unchanged",
          "[ams][happy_hare][hh_v4]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json");
    // No display_name: the unit keeps the name "MMU", so the split is unchanged
    // and only the topologies are refreshed.
    fx["mmu_machine"]["unit_0"].erase("display_name");
    fx["mmu_machine"]["unit_0"]["selector_type"] = "LinearSelector";
    helper.test_parse_mmu_state(fx["mmu_status"]);
    helper.mark_unit(0, "kept");
    connect_with_fixture(helper, client, fx);
    REQUIRE(helper.unit_marker(0) == "kept");
    const auto info = helper.get_system_info();
    CHECK(info.units[0].topology == PathTopology::LINEAR);
    CHECK_FALSE(info.units[0].has_encoder); // no [mmu_unit] encoder configured
}

TEST_CASE("Happy Hare gate_vendor is the lane brand unless the user declared one",
          "[ams][happy_hare][hh_v4]") {
    using nlohmann::json;
    struct Row {
        const char* name;
        const char* declared; // override brand, nullptr = none
        std::vector<json> frames;
        const char* brand0;
    };
    const Row rows[] = {
        {"a vendor lands", nullptr, {{{"gate_vendor", json::array({"QIDI", ""})}}}, "QIDI"},
        {"an empty vendor clears it",
         nullptr,
         {{{"gate_vendor", json::array({"QIDI", ""})}}, {{"gate_vendor", json::array({"", ""})}}},
         ""},
        {"a frame without gate_vendor leaves it alone",
         nullptr,
         {{{"gate_material", json::array({"PLA", "PETG"})}}},
         ""},
        {"an empty vendor keeps a declared brand",
         "Polymaker",
         {{{"gate_vendor", json::array({"QIDI", ""})}}, {{"gate_vendor", json::array({"", ""})}}},
         "Polymaker"},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        if (row.declared) {
            helix::ams::FilamentSlotOverride o;
            o.brand = row.declared;
            helper.set_gate_override(0, o);
        }
        for (const auto& f : row.frames)
            helper.feed_mmu_status(f);
        CHECK(helper.get_slot_info(0).brand == row.brand0);
        CHECK(helper.get_slot_info(1).brand.empty());
    }
}

TEST_CASE("Happy Hare writes VENDOR to the gate map on v4 only", "[ams][happy_hare][hh_v4]") {
    for (const bool v4 : {true, false}) {
        INFO((v4 ? "v4" : "v3"));
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        if (v4)
            helper.feed_mmu_status({{"tangle_prevention", {{"enabled", false}}}});

        SlotInfo info;
        info.color_rgb = 0xFF0000;
        info.brand = "Prusament";
        helix::test::apply_edit(helper, 0, info);

        if (v4) {
            CHECK(helper.has_gcode("MMU_GATE_MAP GATE=0 COLOR=FF0000 VENDOR=Prusament"));
        } else {
            CHECK(helper.has_gcode("MMU_GATE_MAP GATE=0 COLOR=FF0000"));
            CHECK_FALSE(helper.has_gcode_containing("VENDOR"));
        }
    }
}

TEST_CASE("Happy Hare v4 takes a drying humidity target, v3 does not",
          "[ams][happy_hare][hh_v4][dryer_humidity]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;

    SECTION("v3: no capability, and the temp + time command is unchanged") {
        helper.initialize_test_gates(4);
        helper.set_config_defaults_for_test();
        HappyHareTestAccess::dryer_info(helper).supported = true;
        CHECK_FALSE(helper.get_dryer_info(0).supports_humidity_target);
        // A default left over in the dryer state is not reported without the capability.
        HappyHareTestAccess::dryer_info(helper).default_humidity_target = 25.0f;
        CHECK_FALSE(helper.get_dryer_info(0).default_humidity_target);

        helper.clear_captured_gcodes();
        const auto refused = helper.start_drying_to_humidity(50.0f, 60, 15.0f, -1, 0);
        CHECK(refused.result == AmsResult::NOT_SUPPORTED);
        CHECK(helper.captured_gcodes.empty());

        CHECK(helper.start_drying(50.0f, 60, -1, 0).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60"});
    }
    SECTION("v4 single unit") {
        connect_with_fixture(
            helper, client, helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json"));
        HappyHareTestAccess::dryer_info(helper).supported = true;
        CHECK(helper.get_dryer_info(0).supports_humidity_target);
        // heater_default_dry_humidity from the unit's parameters section.
        REQUIRE(helper.get_dryer_info(0).default_humidity_target);
        CHECK(*helper.get_dryer_info(0).default_humidity_target == 25.0f);

        helper.clear_captured_gcodes();
        CHECK(helper.start_drying(50.0f, 60, -1, 0).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60"});

        CHECK(helper.stop_drying(0).success());
        helper.clear_captured_gcodes();
        CHECK(helper.start_drying_to_humidity(50.0f, 60, 15.0f, -1, 0).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60 HUMIDITY=15"});
    }
    SECTION("v4: a duration change restarts the cycle with the same humidity target") {
        connect_with_fixture(
            helper, client, helix::test::load_happy_hare_fixture("happy_hare_v4_single_unit.json"));
        HappyHareTestAccess::dryer_info(helper).supported = true;
        CHECK(helper.start_drying_to_humidity(50.0f, 60, 15.0f, -1, 0).success());
        HappyHareTestAccess::dryer_info(helper).target_temp_c = 50.0f;

        helper.clear_captured_gcodes();
        CHECK(helper.update_drying(-1.0f, 90, -1, 0).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER STOP=1",
                                       "MMU_HEATER DRY=1 TEMP=50 TIMER=90 HUMIDITY=15"});

        // A plain start afterwards carries no target forward.
        CHECK(helper.stop_drying(0).success());
        helper.clear_captured_gcodes();
        CHECK(helper.start_drying(50.0f, 60, -1, 0).success());
        CHECK(helper.update_drying(-1.0f, 90, -1, 0).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60", "MMU_HEATER STOP=1",
                                       "MMU_HEATER DRY=1 TEMP=50 TIMER=90"});
    }
    SECTION("v4 two units: the target sits before the unit and gate suffix") {
        const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
        helper.test_parse_mmu_state(fx["mmu_status"]);
        connect_with_fixture(helper, client, fx);
        HappyHareTestAccess::dryer_info(helper).supported = true;

        helper.clear_captured_gcodes();
        CHECK(helper.start_drying_to_humidity(50.0f, 60, 15.0f, -1, 1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{
                  "MMU_HEATER DRY=1 TEMP=50 TIMER=60 HUMIDITY=15 UNIT=1 GATES=6,7,8,9"});
    }
}

TEST_CASE("Happy Hare v4 drives a chosen set of gates on per-gate heaters",
          "[ams][happy_hare][hh_v4][dry_gates]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    // Unit 0 (ERCF) shares one heater across its gates; unit 1 (Box Turtle) has one per
    // gate and an eSpooler.
    auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    fx["mmu_machine"]["unit_0"]["filament_heater"] = "heater_generic ercf_heater";
    helper.test_parse_mmu_state(fx["mmu_status"]);
    connect_with_fixture(helper, client, fx);
    HappyHareTestAccess::dryer_info(helper).supported = true;

    SECTION("only the per-gate unit offers a selection, and rotation where spools turn") {
        CHECK(helper.get_dryer_info(1).supports_gate_selection);
        CHECK(helper.get_dryer_info(1).supports_rotation);
        CHECK_FALSE(helper.get_dryer_info(0).supports_gate_selection);
        CHECK_FALSE(helper.get_dryer_info(0).supports_rotation);
    }
    SECTION("no eSpooler, no rotation") {
        helper.test_parse_mmu_state(
            {{"espooler", nlohmann::json::array({"", "", "", "", "", "", "", "", "", ""})}});
        CHECK(helper.get_dryer_info(1).supports_gate_selection);
        CHECK_FALSE(helper.get_dryer_info(1).supports_rotation);
        DryingRequest req{.temp_c = 50.0f, .duration_min = 60, .unit = 1, .gates = {7}};
        req.rotate = true;
        helper.clear_captured_gcodes();
        CHECK(helper.start_drying_request(req).result == AmsResult::NOT_SUPPORTED);
        CHECK(helper.captured_gcodes.empty());
    }
    SECTION("a selection names its gates") {
        helper.clear_captured_gcodes();
        CHECK(helper
                  .start_drying_request(
                      {.temp_c = 50.0f, .duration_min = 60, .unit = 1, .gates = {8, 7, 8}})
                  .success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=7,8"});
    }
    SECTION("humidity and rotation ride the same command") {
        DryingRequest req{
            .temp_c = 50.0f, .duration_min = 60, .humidity_pct = 15.0f, .unit = 1, .gates = {6, 9}};
        req.rotate = true;
        helper.clear_captured_gcodes();
        CHECK(helper.start_drying_request(req).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{
                  "MMU_HEATER DRY=1 TEMP=50 TIMER=60 HUMIDITY=15 ROTATE=1 UNIT=1 GATES=6,9"});
    }
    SECTION("rotation with no selection names every gate on the unit") {
        DryingRequest req{.temp_c = 50.0f, .duration_min = 60, .unit = 1};
        req.rotate = true;
        helper.clear_captured_gcodes();
        CHECK(helper.start_drying_request(req).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{
                  "MMU_HEATER DRY=1 TEMP=50 TIMER=60 ROTATE=1 UNIT=1 GATES=6,7,8,9"});
    }
    SECTION("no selection sends exactly the whole-unit start") {
        helper.clear_captured_gcodes();
        CHECK(helper.start_drying_request({.temp_c = 50.0f, .duration_min = 60, .unit = 1})
                  .success());
        CHECK(helper
                  .start_drying_request(
                      {.temp_c = 50.0f, .duration_min = 60, .humidity_pct = 15.0f, .unit = 1})
                  .success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{
                  "MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=6,7,8,9",
                  "MMU_HEATER DRY=1 TEMP=50 TIMER=60 HUMIDITY=15 UNIT=1 GATES=6,7,8,9"});
    }
    SECTION("a shared-heater unit refuses a selection") {
        helper.clear_captured_gcodes();
        CHECK(helper
                  .start_drying_request(
                      {.temp_c = 50.0f, .duration_min = 60, .unit = 0, .gates = {1}})
                  .result == AmsResult::NOT_SUPPORTED);
        CHECK(helper.stop_drying_gates(0, {1}).result == AmsResult::NOT_SUPPORTED);
        CHECK(helper.captured_gcodes.empty());
    }
    SECTION("gates on no unit are dropped, and none left sends nothing") {
        helper.clear_captured_gcodes();
        CHECK_FALSE(helper
                        .start_drying_request(
                            {.temp_c = 50.0f, .duration_min = 60, .unit = 1, .gates = {42, -1}})
                        .success());
        CHECK(helper.captured_gcodes.empty());
        CHECK(helper
                  .start_drying_request(
                      {.temp_c = 50.0f, .duration_min = 60, .unit = 1, .gates = {42, 9}})
                  .success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=9"});
    }
    SECTION("stopping a selection cancels only those gates") {
        helper.clear_captured_gcodes();
        CHECK(helper.stop_drying_gates(1, {8}).success());
        CHECK(helper.stop_drying_gates(1, {}).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER STOP=1 UNIT=1 GATES=8",
                                       "MMU_HEATER STOP=1 UNIT=1 GATES=6,7,8,9"});
    }
    SECTION("a stopped gate leaves the selection a temperature change reaches") {
        CHECK(helper
                  .start_drying_request(
                      {.temp_c = 50.0f, .duration_min = 60, .unit = 1, .gates = {7, 8}})
                  .success());
        REQUIRE(helper.stop_drying_gates(1, {8}).success());
        helper.clear_captured_gcodes();
        CHECK(helper.update_drying(55.0f, -1, -1, 1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER TEMP=55 UNIT=1 GATES=7"});
        HappyHareTestAccess::dryer_info(helper).target_temp_c = 55.0f;
        helper.clear_captured_gcodes();
        CHECK(helper.update_drying(-1.0f, 90, -1, 1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER STOP=1 UNIT=1 GATES=6,7,8,9",
                                       "MMU_HEATER DRY=1 TEMP=55 TIMER=90 UNIT=1 GATES=7"});
    }
    SECTION("a cycle the printer finishes forgets its gates") {
        CHECK(helper
                  .start_drying_request(
                      {.temp_c = 50.0f, .duration_min = 60, .unit = 1, .gates = {7, 8}})
                  .success());
        // A frame from before the start is not the end of the cycle.
        helper.test_parse_mmu_state(
            {{"drying_state", nlohmann::json::array({"", "", "", "", "", "", "", "", "", ""})}});
        helper.clear_captured_gcodes();
        CHECK(helper.update_drying(55.0f, -1, -1, 1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER TEMP=55 UNIT=1 GATES=7,8"});

        helper.test_parse_mmu_state(
            {{"drying_state",
              nlohmann::json::array({"", "", "", "", "", "", "", "active", "queued", ""})}});
        helper.test_parse_mmu_state(
            {{"drying_state",
              nlohmann::json::array({"", "", "", "", "", "", "", "complete", "cancelled", ""})}});
        helper.clear_captured_gcodes();
        CHECK(helper.update_drying(55.0f, -1, -1, 1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER TEMP=55 UNIT=1 GATES=6,7,8,9"});
    }
    SECTION("a restart and a retarget keep the selected gates") {
        DryingRequest req{.temp_c = 50.0f, .duration_min = 60, .unit = 1, .gates = {7, 8}};
        req.rotate = true;
        CHECK(helper.start_drying_request(req).success());
        HappyHareTestAccess::dryer_info(helper).target_temp_c = 50.0f;

        helper.clear_captured_gcodes();
        CHECK(helper.update_drying(55.0f, -1, -1, 1).success());
        CHECK(helper.update_drying(-1.0f, 90, -1, 1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{
                  "MMU_HEATER TEMP=55 UNIT=1 GATES=7,8", "MMU_HEATER STOP=1 UNIT=1 GATES=6,7,8,9",
                  "MMU_HEATER DRY=1 TEMP=50 TIMER=90 ROTATE=1 UNIT=1 GATES=7,8"});

        // A stop ends the selection with the cycle, and a whole-unit cycle carries none.
        CHECK(helper.stop_drying(1).success());
        helper.clear_captured_gcodes();
        CHECK(helper.update_drying(55.0f, -1, -1, 1).success());
        CHECK(helper.start_drying(50.0f, 60, -1, 1).success());
        CHECK(helper.update_drying(55.0f, -1, -1, 1).success());
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_HEATER TEMP=55 UNIT=1 GATES=6,7,8,9",
                                       "MMU_HEATER DRY=1 TEMP=50 TIMER=60 UNIT=1 GATES=6,7,8,9",
                                       "MMU_HEATER TEMP=55 UNIT=1 GATES=6,7,8,9"});
    }
}

TEST_CASE("Happy Hare v3 never offers a gate selection", "[ams][happy_hare][dry_gates]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);
    helper.test_apply_heater_config(
        {{"mmu_machine",
          {{"filament_heaters", "heater_generic Lane_0_heater, heater_generic Lane_1_heater"},
           {"environment_sensors", "temperature_sensor Lane_0, temperature_sensor Lane_1"}}}});
    HappyHareTestAccess::dryer_info(helper).supported = true;

    CHECK_FALSE(helper.get_dryer_info(0).supports_gate_selection);
    helper.clear_captured_gcodes();
    CHECK(helper.start_drying_request({.temp_c = 50.0f, .duration_min = 60, .gates = {1}}).result ==
          AmsResult::NOT_SUPPORTED);
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("Each slot carries its gate's drying state", "[ams][happy_hare][dry_gates]") {
    using helix::printer::ZoneDryingState;
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    helper.test_parse_mmu_state(fx["mmu_status"]);
    connect_with_fixture(helper, client, fx);

    auto state_of = [&helper](int gate) {
        const auto info = helper.get_system_info();
        const SlotInfo* slot = info.get_slot_global(gate);
        REQUIRE(slot != nullptr);
        return slot->drying_state;
    };

    // A short list: gates past its end read Idle rather than shifting or throwing.
    helper.test_parse_mmu_state(
        {{"drying_state",
          nlohmann::json::array({"", "", "", "", "", "", "active", "queued", "complete"})}});
    CHECK(state_of(0) == ZoneDryingState::Idle);
    CHECK(state_of(6) == ZoneDryingState::Active);
    CHECK(state_of(7) == ZoneDryingState::Queued);
    CHECK(state_of(8) == ZoneDryingState::Complete);
    CHECK(state_of(9) == ZoneDryingState::Idle);

    helper.test_parse_mmu_state({{"drying_state", nlohmann::json::array({"", "", "", "", "", "", "",
                                                                         "", "", "cancelled"})}});
    CHECK(state_of(6) == ZoneDryingState::Idle);
    CHECK(state_of(9) == ZoneDryingState::Cancelled);
}

TEST_CASE("A drying_state-only frame reaches the UI", "[ams][happy_hare][dry_gates]") {
    using helix::printer::ZoneDryingState;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    int state_events = 0;
    helper.set_event_callback([&](const std::string& name, const std::string&) {
        state_events += name == helix::AmsBackend::EVENT_STATE_CHANGED ? 1 : 0;
    });

    helper.feed_mmu_status({{"drying_state", nlohmann::json::array({"", "active", "", ""})}});

    // EVENT_STATE_CHANGED is what makes AmsState resync every slot's subjects.
    CHECK(state_events == 1);
    CHECK(helper.get_system_info().get_slot_global(1)->drying_state == ZoneDryingState::Active);
}

TEST_CASE("Per-gate zones each carry their own unit's dryer", "[ams][happy_hare][dry_gates]") {
    QueryCapturingClient client;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(nullptr, &client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    const auto fx = helix::test::load_happy_hare_fixture("happy_hare_v4_two_unit.json");
    helper.test_parse_mmu_state(fx["mmu_status"]);
    connect_with_fixture(helper, client, fx);
    HappyHareTestAccess::dryer_info(helper).supported = true;

    const auto zones = helper.get_environment_zones(1);
    REQUIRE_FALSE(zones.empty());
    for (const auto& z : zones) {
        CHECK(z.dryer.supports_gate_selection);
    }
}

TEST_CASE("Happy Hare TTG remap is the routing an unremapped print runs with",
          "[ams][happy_hare][tool_mapping][routing]") {
    // Gate 1 out of service, gate 3 standing in for it via MMU_TTG_MAP. A print
    // using T1 must seed gate 3, so print start sends no MMU_TTG_MAP undoing it.
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    nlohmann::json mmu_data = {{"gate", -1},
                               {"tool", -1},
                               {"filament", "Unloaded"},
                               {"action", "Idle"},
                               {"gate_status", {1, 0, 1, 1}},
                               {"gate_color_rgb", {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00}},
                               {"gate_material", {"PLA", "PLA", "PLA", "PLA"}},
                               {"ttg_map", {0, 3, 2, 1}}};
    helper.test_parse_mmu_state(mmu_data);
    helix::ui::UpdateQueue::instance().drain();

    const auto live = helper.get_tool_mapping();
    REQUIRE(live.size() == 4);
    REQUIRE(live[1] == 3);
    CHECK(helper.firmware_default_routing().head(1) == 3);
    CHECK(helper.firmware_default_routing().head(3) == 1);

    helix::GcodeToolInfo t1{};
    t1.tool_index = 1;
    t1.color_rgb = 0x808080;
    t1.color_known = false;
    const std::vector<helix::GcodeToolInfo> tools{t1};
    auto& ams = helix::AmsState::instance();
    const auto slots = ams.collect_available_slots();
    const auto seed = ams.seed_tool_mappings(tools, slots, false);
    REQUIRE(seed.size() == 1);
    CHECK(seed[0].mapped_slot == live[1]);
    CHECK_FALSE(helix::PreflightValidator::validate(tools, slots, seed,
                                                    ams.collect_firmware_routing(), false, false)
                    .has_block());
}

// Happy Hare re-derives the per-gate LOADED stamp from the cached gate_status
// array on every printer.mmu frame. It does not claim per-slot authority:
// mmu.gate and mmu.filament are firmware's own values parsed from one object, so
// the aggregate pair is the authority and the per-gate status derives from it.
// Believing the derivation instead would drop the highlight on a gate that ran
// out (gate_status 0) while still feeding the toolhead (#1199).
namespace {

/// gate_status values: -1 unknown, 0 empty, 1 available, 2 from_buffer.
nlohmann::json hh_gates(const std::vector<int>& gate_status, int gate, const char* filament) {
    return nlohmann::json{{"gate_status", gate_status}, {"gate", gate}, {"filament", filament}};
}

} // namespace

TEST_CASE("Happy Hare leaves an unloaded from_buffer gate as FROM_BUFFER",
          "[ams][happy_hare][1199]") {
    // The upgrade must be about the loaded gate, not about from_buffer gates in
    // general — otherwise every buffered gate would read as seated.
    helix::AmsBackendHappyHareTestHelper hh;
    hh.feed_mmu_status(hh_gates({1, 1, 2, 1}, 0, "Loaded"));

    CHECK(hh.get_slot_info(0).status == SlotStatus::LOADED);
    CHECK(hh.get_slot_info(2).status == SlotStatus::FROM_BUFFER);
}

// ============================================================================
// Hole 2: gate / filament arrive without gate_status
// ============================================================================

TEST_CASE("Happy Hare moves the LOADED stamp on a gate-only delta", "[ams][happy_hare][1199]") {
    helix::AmsBackendHappyHareTestHelper hh;
    hh.feed_mmu_status(hh_gates({1, 1, 1, 1}, 0, "Loaded"));
    REQUIRE(hh.get_slot_info(0).status == SlotStatus::LOADED);

    SECTION("a toolchange frame carrying only gate follows the firmware") {
        // The frame a real toolchange produces: gate_status did not change, so
        // Moonraker does not resend it. The stamp still has to move.
        hh.feed_mmu_status(nlohmann::json{{"gate", 3}});

        REQUIRE(hh.get_system_info().current_slot == 3);
        CHECK(hh.get_slot_info(3).status == SlotStatus::LOADED);
        CHECK(hh.get_slot_info(0).status == SlotStatus::AVAILABLE);
    }

    SECTION("an unload frame carrying only filament clears it") {
        hh.feed_mmu_status(nlohmann::json{{"filament", "Unloaded"}});

        REQUIRE_FALSE(hh.is_filament_loaded());
        for (int i = 0; i < 4; ++i) {
            CHECK(hh.get_slot_info(i).status != SlotStatus::LOADED);
        }
    }

    SECTION("a later gate_status delta does not resurrect the old gate") {
        hh.feed_mmu_status(nlohmann::json{{"gate", 3}});
        hh.feed_mmu_status(nlohmann::json{{"gate_status", nlohmann::json::array({1, 0, 1, 1})}});

        CHECK(hh.get_slot_info(3).status == SlotStatus::LOADED);
        CHECK(hh.get_slot_info(0).status == SlotStatus::AVAILABLE);
        CHECK(hh.get_slot_info(1).status == SlotStatus::EMPTY);
    }
}

// ============================================================================
// Where the aggregate and the per-gate status legitimately disagree
// ============================================================================

TEST_CASE("Happy Hare does not paint an empty gate as loaded", "[ams][happy_hare][1199]") {
    // gate_status 0 after a runout is real information: the gate has nothing
    // left to feed, and load_filament()'s "slot not available" refusal keys on
    // it. Happy Hare can still report that gate as the loaded one, because the
    // filament it already fed is at the toolhead — the same shape as the AD5X
    // #995 runout. This disagreement is precisely why Happy Hare keeps the
    // aggregate rule: switching to the per-gate status would blank the
    // active-lane highlight here.
    helix::AmsBackendHappyHareTestHelper hh;
    hh.feed_mmu_status(hh_gates({0, 1, 1, 1}, 0, "Loaded"));

    CHECK(hh.get_slot_info(0).status == SlotStatus::EMPTY);
    CHECK(hh.slot_is_actively_loaded(0));
    CHECK_FALSE(hh.slot_is_actively_loaded(1));
}

TEST_CASE("Happy Hare bypass and no-gate states stamp nothing", "[ams][happy_hare][1199]") {
    helix::AmsBackendHappyHareTestHelper hh;

    SECTION("bypass (gate -2) is not a gate index") {
        hh.feed_mmu_status(hh_gates({1, 1, 1, 1}, -2, "Loaded"));
        for (int i = 0; i < 4; ++i) {
            CHECK(hh.get_slot_info(i).status != SlotStatus::LOADED);
            CHECK_FALSE(hh.slot_is_actively_loaded(i));
        }
    }

    SECTION("no gate selected (-1) is not a gate index") {
        hh.feed_mmu_status(hh_gates({1, 1, 1, 1}, -1, "Loaded"));
        for (int i = 0; i < 4; ++i) {
            CHECK(hh.get_slot_info(i).status != SlotStatus::LOADED);
        }
    }

    SECTION("a gate index past the end is ignored, not a crash") {
        hh.feed_mmu_status(hh_gates({1, 1, 1, 1}, 9, "Loaded"));
        for (int i = 0; i < 4; ++i) {
            CHECK(hh.get_slot_info(i).status != SlotStatus::LOADED);
        }
    }
}
