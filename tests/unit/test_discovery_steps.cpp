// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_discovery_steps.cpp
 * @brief The discovery-complete step tables: their order, their hw_changed gating and the
 *        print_active decision every wizard and gcode gate in the pass shares.
 *
 * Later steps read what earlier ones stored (hardware before status dispatch, auto-detect
 * before validation, validation before the prompts), so the tables are pinned by name. The
 * core table runs on every build, the firmware included; desktop runs it and then the tail.
 * The walker is exercised against a recording table: running the real steps needs a live
 * printer.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/breadcrumb_capture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/filament_sensor_manager_test_access.h"
#include "../test_helpers/printer_capture.h"
#include "../test_helpers/scoped_env.h"
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "config.h"
#include "core/update_queue.h"
#include "discovery_steps.h"
#include "filament_sensor_manager.h"
#include "filament_temperature_source.h"
#include "hardware_setup_prompter.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::DiscoveryContext;
using helix::DiscoveryStep;
using helix::DiscoveryStepRange;
using nlohmann::json;

namespace {

std::vector<std::string>& ran() {
    static std::vector<std::string> v;
    return v;
}

void record_a(DiscoveryContext&) {
    ran().push_back("a");
}
void record_b(DiscoveryContext&) {
    ran().push_back("b");
}
void record_c(DiscoveryContext&) {
    ran().push_back("c");
}

constexpr DiscoveryStep kRecording[] = {
    {"a", false, record_a, "test_a"},
    {"b", true, record_b, "test_b"},
    {"c", false, record_c, nullptr},
};

struct StepFixture : LVGLTestFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;
    helix::PrinterDiscovery snapshot;
    json status = json::object();
    helix::AsyncLifetimeGuard lifetime;
    helix::HardwareSetupPrompter prompter;

    StepFixture()
        : api(client, get_printer_state()),
          prompter(lifetime, [] { return nullptr; }, [] { return nullptr; }) {
        ran().clear();
    }

    DiscoveryContext context(bool hw_changed, long n = 7) {
        return DiscoveryContext{api,    client,     api.hardware(), snapshot,
                                status, &prompter,  nullptr,        nullptr,
                                n,      hw_changed, false};
    }
};

/// Every step desktop runs, in its order: the core table, then the tail.
std::vector<DiscoveryStep> all_steps() {
    std::vector<DiscoveryStep> steps;
    for (const DiscoveryStepRange range :
         {helix::discovery_core_steps(), helix::discovery_tail_steps()}) {
        steps.insert(steps.end(), range.begin(), range.end());
    }
    return steps;
}

std::vector<std::string> names_of(const std::vector<DiscoveryStep>& steps) {
    std::vector<std::string> names;
    for (const DiscoveryStep& step : steps) {
        names.emplace_back(step.name);
    }
    return names;
}

size_t position_of(const std::vector<std::string>& names, const std::string& name) {
    const auto it = std::find(names.begin(), names.end(), name);
    REQUIRE(it != names.end());
    return static_cast<size_t>(it - names.begin());
}

} // namespace

TEST_CASE("the core discovery steps run in this order", "[discovery_steps]") {
    // Hardware lands in PrinterState before the status replay and the fan roles it
    // reads; the heater heal reads the printer type auto-detect stored.
    std::vector<std::string> core;
    for (const DiscoveryStep& step : helix::discovery_core_steps()) {
        core.emplace_back(step.name);
    }
    const std::vector<std::string> expected = {
        "set_hardware",       "zoffset_persistence", "temp_graph_seed",      "status_dispatch",
        "software_versions",  "auto_detect_printer", "heal_heater_roles",    "safety_limits",
        "helix_plugin_check", "job_queue_fetch",     "settle_light_buttons",
    };
    CHECK(core == expected);
}

TEST_CASE("the tail discovery steps run in this order", "[discovery_steps]") {
    // The prompts come after validation and before its snapshot is saved.
    std::vector<std::string> tail;
    for (const DiscoveryStep& step : helix::discovery_tail_steps()) {
        tail.emplace_back(step.name);
    }
    const std::vector<std::string> expected = {
        "update_checker_connected",
        "about_print_hours",
        "timelapse_events",
        "power_sensor_subscribe",
        "acknowledge_deferred_hardware",
        "validate_hardware",
        "hardware_prompts",
        "save_validation_snapshot",
        "telemetry",
        "spoolman_sync",
        "auto_update_check",
        "moonraker_update_channel",
        "filament_temperatures",
        "remote_screen_webcam",
        "manual_probe_autoopen",
    };
    CHECK(tail == expected);
}

TEST_CASE("desktop's combined order keeps the cross-table dependencies", "[discovery_steps]") {
    const auto names = names_of(all_steps());
    // The status replay writes subjects set_hardware builds.
    CHECK(position_of(names, "set_hardware") < position_of(names, "status_dispatch"));
    // The validator must see the post-preset roles auto-detect and the heal write.
    CHECK(position_of(names, "auto_detect_printer") < position_of(names, "validate_hardware"));
    CHECK(position_of(names, "heal_heater_roles") < position_of(names, "validate_hardware"));
}

TEST_CASE("only the steps that are pure functions of the hardware shape are gated",
          "[discovery_steps]") {
    std::vector<std::string> gated;
    for (const DiscoveryStep& step : all_steps()) {
        CHECK(step.run != nullptr);
        if (step.only_when_hw_changed) {
            gated.emplace_back(step.name);
        }
    }
    CHECK(gated == std::vector<std::string>{"auto_detect_printer", "heal_heater_roles"});
}

TEST_CASE("the discovery breadcrumbs keep their keys", "[discovery_steps]") {
    std::vector<std::string> crumbs;
    for (const DiscoveryStep& step : all_steps()) {
        if (step.breadcrumb) {
            crumbs.emplace_back(step.breadcrumb);
        }
    }
    // The in-step crumbs (pre_set_hw, post_set_hw, post_init_fans) are recorded by
    // set_hardware itself; these are the table's.
    CHECK(crumbs == std::vector<std::string>{"post_status_dispatch", "post_subscribe",
                                             "post_validate", "post_telemetry"});
}

TEST_CASE_METHOD(StepFixture, "a changed hardware shape runs every step", "[discovery_steps]") {
    auto ctx = context(/*hw_changed=*/true);
    helix::run_discovery_steps(DiscoveryStepRange{std::begin(kRecording), std::end(kRecording)},
                               ctx);
    CHECK(ran() == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE_METHOD(StepFixture, "an unchanged shape skips the gated steps but keeps their crumbs",
                 "[discovery_steps]") {
    auto ctx = context(/*hw_changed=*/false, /*n=*/4242);
    helix::run_discovery_steps(DiscoveryStepRange{std::begin(kRecording), std::end(kRecording)},
                               ctx);

    CHECK(ran() == std::vector<std::string>{"a", "c"});

    const auto lines = helix::capture_breadcrumb_lines();
    auto saw = [&lines](const std::string& crumb) {
        for (const std::string& line : lines) {
            if (line.find("disc") != std::string::npos &&
                line.find(crumb + " 4242") != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    CHECK(saw("test_a"));
    CHECK(saw("test_b")); // skipped, still recorded
}

TEST_CASE("the print_active decision: the discovery status can raise it, never lower it",
          "[discovery_steps][print_active]") {
    const json idle = {{"print_stats", {{"state", "standby"}}}};
    const json printing = {{"print_stats", {{"state", "printing"}}}};

    // A fresh connection mid-print: the subject still holds its initial 0.
    CHECK(helix::discovery_print_active(false, printing));
    CHECK(helix::discovery_print_active(true, idle));
    CHECK(helix::discovery_print_active(true, printing));
    CHECK_FALSE(helix::discovery_print_active(false, idle));
    CHECK_FALSE(helix::discovery_print_active(false, json::object()));
}

namespace {

constexpr const char* kCc1Sensor = "filament_switch_sensor filament_sensor";

/// The global Config swapped for a fresh install and restored on scope exit.
class FreshInstallConfig {
  public:
    FreshInstallConfig()
        : config_(*helix::Config::get_instance()), data_(helix::ConfigTestAccess::data(config_)),
          path_(helix::ConfigTestAccess::path(config_)),
          id_(helix::ConfigTestAccess::active_printer_id(config_)),
          dir_((std::filesystem::temp_directory_path() /
                ("helix_preset_roles_" + std::to_string(getpid())))
                   .string()) {
        std::filesystem::create_directories(dir_);
        helix::ConfigTestAccess::path(config_) = dir_ + "/settings.json";
        helix::ConfigTestAccess::active_printer_id(config_) = "default";
        helix::ConfigTestAccess::data(config_) = {
            {"active_printer_id", "default"},
            {"printers",
             {{"default", {{"moonraker_host", "127.0.0.1"}, {"wizard_completed", false}}}}}};
        get_printer_state().set_printer_type_sync("");
    }
    ~FreshInstallConfig() {
        discover_filament_sensors(helix::FilamentSensorManager::instance(), {});
        helix::ConfigTestAccess::data(config_) = data_;
        helix::ConfigTestAccess::path(config_) = path_;
        helix::ConfigTestAccess::active_printer_id(config_) = id_;
        get_printer_state().set_printer_type_sync("");
        std::filesystem::remove_all(dir_);
    }
    helix::Config& config() {
        return config_;
    }

  private:
    helix::Config& config_;
    json data_;
    std::string path_;
    std::string id_;
    std::string dir_;
};

bool cc1_sensor_is_runout() {
    for (const auto& sensor : helix::FilamentSensorManager::instance().get_sensors()) {
        if (sensor.klipper_name == kCc1Sensor) {
            return sensor.role == helix::FilamentSensorRole::RUNOUT;
        }
    }
    return false;
}

/// Sensors discovered and their config loaded before any preset exists, as startup does.
void discover_cc1_sensor_without_preset(helix::PrinterDiscovery& hw) {
    hw.parse_objects(
        helix::test::load_printer_capture("elegoo_centauri_carbon_objects_list").at("objects"));
    auto& fsm = helix::FilamentSensorManager::instance();
    fsm.discover(hw);
    fsm.load_settings();
}

} // namespace

// A preset applied by the auto-detect step lands after the sensors were discovered and their
// config loaded. The CC1's `filament_sensor` has no role by name, so the preset's role has to
// reach the manager in the same session or the home filament tile stays blank until restart.
TEST_CASE_METHOD(StepFixture, "a preset applied during discovery sets the sensor roles at once",
                 "[discovery_steps][preset][sensors]") {
    FreshInstallConfig fresh;
    discover_cc1_sensor_without_preset(api.hardware());
    REQUIRE_FALSE(cc1_sensor_is_runout());

    fresh.config().set_preset("cc1");
    auto ctx = context(/*hw_changed=*/true);
    for (const DiscoveryStep& step : helix::discovery_core_steps()) {
        if (std::string(step.name) == "auto_detect_printer") {
            step.run(ctx);
        }
    }

    CHECK(cc1_sensor_is_runout());
}

// The wizard and the Printer Manager apply a preset through apply_type_choice, which must
// reload the sensors the same way.
TEST_CASE_METHOD(StepFixture, "a printer type chosen by the user sets the sensor roles at once",
                 "[discovery_steps][preset][sensors]") {
    FreshInstallConfig fresh;
    discover_cc1_sensor_without_preset(api.hardware());
    REQUIRE_FALSE(cc1_sensor_is_runout());

    REQUIRE_FALSE(PrinterDetector::apply_type_choice(&fresh.config(), "Elegoo Centauri Carbon",
                                                     api.hardware())
                      .empty());

    CHECK(cc1_sensor_is_runout());
}

// An install whose wizard finished and whose printer type is saved gets the preset's sensor
// block merged on the next discovery; the sensors must pick it up without a restart.
TEST_CASE_METHOD(StepFixture, "a migrated preset sensor block takes effect in the same session",
                 "[discovery_steps][preset][sensors]") {
    FreshInstallConfig fresh;
    discover_cc1_sensor_without_preset(api.hardware());
    REQUIRE_FALSE(cc1_sensor_is_runout());

    helix::Config& config = fresh.config();
    config.set<bool>(config.df() + "wizard_completed", true);
    config.set<std::string>(config.df() + helix::wizard::PRINTER_TYPE, "Elegoo Centauri Carbon");

    auto ctx = context(/*hw_changed=*/true);
    for (const DiscoveryStep& step : helix::discovery_core_steps()) {
        if (std::string(step.name) == "auto_detect_printer") {
            step.run(ctx);
        }
    }

    CHECK(cc1_sensor_is_runout());
}

TEST_CASE_METHOD(StepFixture, "the filament_temperatures step publishes the firmware's table",
                 "[discovery_steps][filament-temps]") {
    helix::filament_temps::clear_filament_temperatures();
    api.set_config_files({
        {"snapmaker/filament_para_standard_04.json",
         R"({"generic_PLA_generic_load_temp": 250, "generic_PLA_generic_unload_temp": 245})"},
        {"snapmaker/filament_para_standard_02.json", R"({"generic_PETG_generic_load_temp": 270})"},
        {"snapmaker/filament_para_standard_06.json", "{}"},
        {"snapmaker/filament_para_standard_08.json", "{}"},
        {"snapmaker/filament_para_high_flow_04.json", "{}"},
    });
    api.hardware().parse_objects(json::array({"extruder", "filament_parameters"}));

    auto ctx = context(/*hw_changed=*/true);
    bool found = false;
    for (const DiscoveryStep& step : helix::discovery_tail_steps()) {
        if (std::string(step.name) == "filament_temperatures") {
            found = true;
            step.run(ctx);
        }
    }
    REQUIRE(found);

    helix::SlotInfo pla;
    pla.brand = "Generic";
    pla.material = "PLA";
    auto t = helix::filament_temps::lookup_filament_temperatures(pla);
    REQUIRE(t.has_value());
    CHECK(t->load_c.value() == 250);
    CHECK(t->unload_c.value() == 245);
    helix::filament_temps::clear_filament_temperatures();
}

TEST_CASE_METHOD(StepFixture, "the plugin probe publishes the plugin's features map",
                 "[discovery_steps][plugin_feature]") {
    auto run_probe = [this]() {
        auto ctx = context(/*hw_changed=*/false);
        bool ran = false;
        for (const DiscoveryStep& step : helix::discovery_core_steps()) {
            if (std::string(step.name) == "helix_plugin_check") {
                step.run(ctx);
                ran = true;
            }
        }
        REQUIRE(ran);
        helix::ui::UpdateQueue::instance().drain();
    };
    const auto& plugin = get_printer_state().plugin_status_state();

    {
        helix::ScopedEnv installed("HELIX_MOCK_HELIX_PLUGIN", "1");
        run_probe();
    }
    // The installed subject is not initialised in this fixture, so read the map itself.
    REQUIRE(plugin.features().count("convert_3mf") == 1);
    CHECK(helix::plugin_feature_state(true, plugin.features(), "convert_3mf", 1) ==
          helix::PluginFeatureState::Ready);

    {
        helix::ScopedEnv absent("HELIX_MOCK_HELIX_PLUGIN", nullptr);
        run_probe();
    }
    CHECK(plugin.features().empty());
}
