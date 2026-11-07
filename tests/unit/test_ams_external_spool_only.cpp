// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_external_spool_only.cpp
 * @brief A backend whose hardware feeds only the external spool presents no
 *        filament system, while its operations stay observable.
 *
 * Run with: ./build/bin/helix-tests "[ams][external-spool-only]"
 */

#include "ui_print_start_controller.h"
#include "ui_wizard_ams_identify.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/filament_sensor_manager_test_access.h"
#include "../test_helpers/force_bypass_controls_guard.h"
#include "../test_helpers/print_start_controller_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "print_start_inputs.h"
#include "printer_discovery.h"
#include "printer_manager_chips.h"
#include "printer_state.h"
#include "tour_steps.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A four-slot mock that can report the external-spool-only state, and an
/// action and loaded flag of the test's choosing on top of it.
class ExternalSpoolOnlyMock : public AmsBackendMock {
  public:
    ExternalSpoolOnlyMock() : AmsBackendMock(4) {}

    [[nodiscard]] AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        info.external_spool_only = external_spool_only;
        info.supports_bypass = supports_bypass;
        info.action = action;
        info.filament_loaded = filament_loaded;
        return info;
    }

    bool external_spool_only = true;
    bool supports_bypass = true;
    AmsAction action = AmsAction::IDLE;
    bool filament_loaded = false;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "an external-spool-only backend presents no filament system",
                 "[ams][external-spool-only][1904]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    auto mock = std::make_unique<ExternalSpoolOnlyMock>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_operation_delay(0);
    ams.set_backend(std::move(mock));
    mock_ptr->start();
    // The mock's own slots are what a presented system would show.
    REQUIRE(mock_ptr->get_system_info().total_slots == 4);

    ams.sync_from_backend();

    SECTION("every subject that says a system exists reads as none") {
        CHECK(lv_subject_get_int(ams.get_ams_type_subject()) == static_cast<int>(AmsType::NONE));
        CHECK(lv_subject_get_int(ams.get_is_filament_system_subject()) == 0);
        CHECK(lv_subject_get_int(ams.get_is_tool_changer_subject()) == 0);
        CHECK(lv_subject_get_int(ams.get_slot_count_subject()) == 0);
        CHECK(lv_subject_get_int(ams.get_supports_bypass_subject()) == 0);
        CHECK(lv_subject_get_int(ams.get_has_device_ops_subject()) == 0);
        CHECK(std::string(lv_subject_get_string(ams.get_ams_system_name_subject())).empty());
        CHECK_FALSE(ams.is_available());
    }

    SECTION("forced bypass controls still offer no toggle") {
        helix::test::ForceBypassControlsGuard force;
        REQUIRE(SettingsManager::instance().get_ams_force_bypass_controls());
        mock_ptr->supports_bypass = false;
        ams.sync_from_backend();
        CHECK(lv_subject_get_int(ams.get_supports_bypass_subject()) == 0);

        // The override is what lights it for a presented system.
        mock_ptr->external_spool_only = false;
        ams.sync_from_backend();
        CHECK(lv_subject_get_int(ams.get_supports_bypass_subject()) == 1);
    }

    SECTION("the operation it runs stays observable") {
        mock_ptr->action = AmsAction::UNLOADING;
        mock_ptr->filament_loaded = true;
        ams.sync_from_backend();
        CHECK(lv_subject_get_int(ams.get_ams_action_subject()) ==
              static_cast<int>(AmsAction::UNLOADING));
        CHECK(lv_subject_get_int(ams.get_filament_loaded_subject()) == 1);
    }

    SECTION("a unit coming online presents the system again") {
        mock_ptr->external_spool_only = false;
        ams.sync_from_backend();
        CHECK(lv_subject_get_int(ams.get_ams_type_subject()) != static_cast<int>(AmsType::NONE));
        CHECK(lv_subject_get_int(ams.get_slot_count_subject()) == 4);
        CHECK(lv_subject_get_int(ams.get_supports_bypass_subject()) == 1);
        CHECK(lv_subject_get_int(ams.get_has_device_ops_subject()) == 1);
        CHECK(ams.is_available());
    }

    mock_ptr->stop();
    ams.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "surfaces that ask whether a filament system exists follow the backend's answer",
                 "[ams][external-spool-only][1904]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    auto mock = std::make_unique<ExternalSpoolOnlyMock>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_operation_delay(0);
    ams.set_backend(std::move(mock));
    mock_ptr->start();

    // One extruder in the printer's config: the fit check's own tool count.
    PrinterDiscovery discovery;
    discovery.parse_fit_limits(nlohmann::json{{"extruder", {{"max_temp", "300"}}}});
    get_printer_state().set_hardware(discovery);

    auto refresh = [&](bool external_spool_only) {
        mock_ptr->external_spool_only = external_spool_only;
        AmsStateTestAccess::invalidate_system_info(ams);
        ams.sync_from_backend();
    };
    auto manages_filament = [] {
        helix::ui::PrintStartController controller(get_printer_state(), nullptr);
        return PrintStartControllerTestAccess::gather(controller).ams_manages_filament;
    };

    SECTION("external spool only: no system anywhere") {
        refresh(true);
        CHECK_FALSE(helix::tour::hardware_has_ams());
        CHECK(WizardAmsIdentifyStep().should_skip());
        const auto fit = current_fit_machine();
        REQUIRE(fit.has_value());
        CHECK(fit->tool_count == std::optional<int>(1));
        CHECK_FALSE(manages_filament());
    }

    SECTION("a presented system everywhere") {
        refresh(false);
        CHECK(helix::tour::hardware_has_ams());
        CHECK_FALSE(WizardAmsIdentifyStep().should_skip());
        const auto fit = current_fit_machine();
        REQUIRE(fit.has_value());
        CHECK_FALSE(fit->tool_count.has_value());
        CHECK(manages_filament());
    }

    get_printer_state().set_hardware(PrinterDiscovery{});
    mock_ptr->stop();
    ams.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a backend reporting no hardware gives its claimed sensor back and drops its chip",
                 "[ams][external-spool-only][1904]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    auto& sensors = FilamentSensorManager::instance();
    sensors.init_subjects();
    FilamentSensorManagerTestAccess::reset(sensors);

    // A K2's objects: the CFS's `box` and the toolhead switch it claims by name.
    PrinterDiscovery discovery;
    discovery.parse_objects(
        nlohmann::json::array({"box", "extruder", "filament_switch_sensor filament_sensor"}));
    REQUIRE(discovery.mmu_type() == AmsType::CFS);
    get_printer_state().set_hardware(discovery);
    discover_filament_sensors(sensors, {"filament_switch_sensor filament_sensor"});
    REQUIRE(sensors.entries().size() == 1);
    const auto toolhead = sensors.entries().front();

    SECTION("with no backend running, the discovery claim holds") {
        ams.clear_backends();
        CHECK_FALSE(ams.filament_hardware_absent());
        CHECK_FALSE(sensors.listed_in_settings(toolhead));
    }

    SECTION("the backend's report decides, both ways") {
        auto mock = std::make_unique<ExternalSpoolOnlyMock>();
        auto* mock_ptr = mock.get();
        ams.set_backend(std::move(mock));
        auto refresh = [&](bool external_spool_only) {
            mock_ptr->external_spool_only = external_spool_only;
            AmsStateTestAccess::invalidate_system_info(ams);
            ams.sync_from_backend();
        };

        refresh(true);
        CHECK(ams.filament_hardware_absent());
        CHECK(sensors.listed_in_settings(toolhead));
        CHECK(gather_capability_chip_facts().filament_systems.empty());

        refresh(false);
        CHECK_FALSE(ams.filament_hardware_absent());
        CHECK_FALSE(sensors.listed_in_settings(toolhead));
        REQUIRE(gather_capability_chip_facts().filament_systems.size() == 1);
        // The count names what it counts in the backend's noun (the mock's is Gate).
        CHECK(gather_capability_chip_facts().filament_systems[0].slot_count == "4 gates");
        ams.clear_backends();
    }

    FilamentSensorManagerTestAccess::reset(sensors);
    get_printer_state().set_hardware(PrinterDiscovery{});
    ams.deinit_subjects();
}
