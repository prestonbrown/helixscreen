// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_telemetry_contract.cpp
 * @brief Client half of the telemetry contract.
 *
 * Every event the client sends is built here from fixed inputs and compared
 * to tests/fixtures/telemetry/<event>.json. The worker's contract test
 * (server/telemetry-worker/src/__tests__/contract.test.ts) reads the same
 * files and requires each field to be written to Analytics Engine or listed
 * as R2-only, so renaming a field on either side turns one of them red.
 *
 * `HELIX_UPDATE_GOLDENS=1 ./build/bin/helix-tests "[contract]"` rewrites the
 * goldens instead of comparing; review the diff before committing it.
 */

#include "ui_update_queue.h"

#include "../test_helpers/display_manager_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_moonraker_api.h"
#include "../test_helpers/scoped_moonraker_client.h"
#include "../test_helpers/telemetry_test_fixture.h"
#include "../ui_test_utils.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

namespace {

std::string golden_dir() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    const std::string root = pos != std::string::npos ? src.substr(0, pos) + "/" : "";
    return root + "tests/fixtures/telemetry/";
}

/// The printer every hardware-reading event describes: each section the
/// session and hardware_profile events can carry is present, so the goldens
/// show every field a connected printer sends.
helix::PrinterDiscovery contract_printer() {
    helix::PrinterDiscovery hw;
    const std::vector<std::string> objects = {
        "configfile",
        "mcu",
        "mcu ebb36",
        "toolhead",
        "extruder",
        "heater_bed",
        "heater_generic chamber",
        "fan",
        "heater_fan hotend_fan",
        "controller_fan electronics",
        "stepper_x",
        "stepper_y",
        "tmc2209 stepper_x",
        "probe",
        "bed_mesh",
        "quad_gantry_level",
        "adxl345",
        "neopixel status_led",
        "filament_switch_sensor runout",
        "temperature_sensor chamber",
        "exclude_object",
        "firmware_retraction",
        "mmu",
        "gcode_macro PRINT_START",
        "gcode_macro CLEAN_NOZZLE",
        "gcode_macro HEAT_SOAK",
        "gcode_macro PURGE_LINE",
    };
    hw.parse_objects(json(objects));
    hw.set_printer_objects(objects);
    hw.set_kinematics("corexy");
    hw.set_mcu("stm32f446xx");
    hw.set_mcu_list({"stm32f446xx", "stm32g0b1xx"});
    hw.set_software_version("v0.12.0-300");
    hw.set_moonraker_version("v0.9.3-20");
    hw.set_os_version("Debian GNU/Linux 12 (bookworm)");
    BuildVolume volume;
    volume.x_max = 350.0f;
    volume.y_max = 350.0f;
    volume.z_max = 340.0f;
    hw.set_build_volume(volume);
    return hw;
}

/// A mock client whose discovered hardware is contract_printer(), at a
/// loopback Moonraker address.
class ContractClient : public MoonrakerClientMock {
  public:
    helix::PrinterDiscovery hardware() const override {
        return contract_printer();
    }
    const std::string& get_last_url() const override {
        return url_;
    }

  private:
    std::string url_ = "ws://127.0.0.1:7125/websocket";
};

class ContractDisplayBackend : public DisplayBackend {
  public:
    lv_display_t* create_display(int, int) override {
        return nullptr;
    }
    lv_indev_t* create_input_pointer() override {
        return nullptr;
    }
    DisplayBackendType type() const override {
        return DisplayBackendType::DRM;
    }
    const char* name() const override {
        return "contract";
    }
    bool is_available() const override {
        return true;
    }
};

class ContractBacklight : public BacklightBackend {
  public:
    bool set_brightness(int) override {
        return true;
    }
    int get_brightness() const override {
        return 100;
    }
    bool is_available() const override {
        return true;
    }
    const char* name() const override {
        return "contract";
    }
};

/// A device's display for a scope: an 800x480 DRM panel with a backlight and
/// hardware blanking, published as DisplayManager::instance() the way init()
/// publishes the real one. The display fields of session and hardware_profile
/// exist only while a DisplayManager does.
class ScopedContractDisplay {
  public:
    ScopedContractDisplay() : previous_(DisplayManager::instance()) {
        DisplayManagerTestAccess::set_backend(dm_, std::make_unique<ContractDisplayBackend>());
        DisplayManagerTestAccess::set_backlight(dm_, std::make_unique<ContractBacklight>());
        DisplayManagerTestAccess::set_use_hardware_blank(dm_, true);
        DisplayManagerTestAccess::set_size(dm_, 800, 480);
        DisplayManagerTestAccess::set_active_instance(&dm_);
    }
    ~ScopedContractDisplay() {
        DisplayManagerTestAccess::set_active_instance(previous_);
    }

    ScopedContractDisplay(const ScopedContractDisplay&) = delete;
    ScopedContractDisplay& operator=(const ScopedContractDisplay&) = delete;

  private:
    DisplayManager dm_;
    DisplayManager* previous_;
};

/// One golden value that does not come from the test's inputs, and the fixed
/// value the golden holds in its place. A pin replaces only a value of its own
/// JSON type (an array: of its element's type), so a field that changes type
/// still differs from the golden.
struct Pinned {
    const char* event; ///< event name, or "*" for every event
    const char* path;  ///< dotted path inside the event
    json value;
    /// A host may lack the probe that fills the field, so the golden records the
    /// device's shape whatever this host reports. The cost: the client dropping
    /// such a field goes unnoticed here (a rename is still caught).
    bool insert_if_absent;
};

const std::vector<Pinned>& pinned_values() {
    static const std::vector<Pinned> pins = {
        // Random per install and per boot, and the client's wall clock.
        {"*", "timestamp", "<timestamp>", false},
        {"*", "device_id", "<device_id>", false},
        {"*", "boot_id", "<boot_id>", false},

        // The test binary's own version, platform and board.
        {"*", "app_version", "<build>", false},
        {"*", "app_platform", "<build>", false},
        {"*", "app_board", "<build>", false},
        {"session", "app.version", "<build>", false},
        {"session", "app.platform", "<build>", false},
        {"session", "app.board", "<build>", false},
        {"update_failed", "from_version", "<build>", false},

        // Seconds of real time since init(). The per-panel and connection
        // durations run on the tracker clock the recorder steps, so they stay.
        {"*", "uptime_sec", 0, false},
        {"*", "session_duration_sec", 0, false},

        // Probes of the machine running the test.
        {"session", "app.zip_tool", "<host>", false},
        {"session", "host.arch", "<host>", true},
        {"session", "host.cpu_model", "<host>", true},
        {"session", "host.cpu_cores", 0, true},
        {"session", "host.ram_total_mb", 0, true},
        {"memory_snapshot", "rss_kb", 0, false},
        {"memory_snapshot", "vm_size_kb", 0, false},
        {"memory_snapshot", "vm_data_kb", 0, false},
        {"memory_snapshot", "vm_swap_kb", 0, false},
        {"memory_snapshot", "vm_peak_kb", 0, false},
        {"memory_snapshot", "vm_hwm_kb", 0, false},
        {"memory_snapshot", "private_dirty_kb", 0, true},
        {"memory_snapshot", "private_clean_kb", 0, true},
        {"memory_snapshot", "shared_clean_kb", 0, true},
        {"memory_snapshot", "pss_kb", 0, true},
        {"memory_snapshot", "system_total_mb", 0, false},
        {"memory_snapshot", "system_available_mb", 0, false},
        {"memory_snapshot", "malloc_arena_kb", 0, true},
        {"memory_snapshot", "malloc_uordblks_kb", 0, true},
        {"memory_snapshot", "malloc_fordblks_kb", 0, true},
        {"memory_snapshot", "malloc_hblks", 0, true},
        {"memory_snapshot", "malloc_hblkhd_kb", 0, true},

        // Process-wide settings and managers that other tests in the same
        // process leave in whatever state they ended in.
        {"session", "app.theme", "<state>", false},
        {"session", "app.locale", "<state>", false},
        {"session", "host.screen_locality", "<state>", false},
        {"settings_snapshot", "theme_name", "<state>", false},
        {"settings_snapshot", "theme", "<state>", false},
        {"settings_snapshot", "locale", "<state>", false},
        {"settings_snapshot", "time_format", "<state>", false},
        {"settings_snapshot", "auto_update_channel", 0, false},
        {"settings_snapshot", "brightness_pct", 0, false},
        {"settings_snapshot", "screensaver_timeout_sec", 0, false},
        {"settings_snapshot", "screen_blank_timeout_sec", 0, false},
        {"settings_snapshot", "screensaver_type", "<state>", false},
        {"settings_snapshot", "sound_enabled", false, false},
        {"settings_snapshot", "animations_enabled", false, false},
        {"settings_snapshot", "theme_changed", false, false},
        {"settings_snapshot", "multi_printer", false, false},
        {"settings_snapshot", "home_widgets", json::array({"<state>"}), false},
        {"memory_warning", "print_state", 0, false},
        {"memory_warning", "active_panel", 0, false},
        {"memory_warning", "gcode_renderer_loaded", false, false},
        {"hardware_profile", "sensors.filament", 0, false},
        {"hardware_profile", "sensors.load_cell", 0, false},
        {"hardware_profile", "sensors.temperature_extra", 0, false},
        {"hardware_profile", "sensors.accel", 0, false},
        {"hardware_profile", "tools.count", 0, false},
        {"hardware_profile", "tools.is_multi_tool", false, false},
        {"hardware_profile", "ams.unit_count", 0, false},
        {"hardware_profile", "ams.total_slots", 0, false},
    };
    return pins;
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> keys;
    std::stringstream parts(path);
    std::string key;
    while (std::getline(parts, key, '.')) {
        keys.push_back(key);
    }
    return keys;
}

bool same_kind(const json& a, const json& b) {
    return (a.is_number() && b.is_number()) || a.type() == b.type();
}

/// `placeholder` when `actual` has its type; for an array placeholder, when
/// every element of `actual` has the type of the placeholder's element.
json pinned_value(const json& actual, const json& placeholder) {
    if (placeholder.is_array()) {
        if (!actual.is_array()) {
            return actual;
        }
        for (const auto& element : actual) {
            if (!same_kind(element, placeholder[0])) {
                return actual;
            }
        }
        return placeholder;
    }
    return same_kind(actual, placeholder) ? placeholder : actual;
}

void apply_pin(json& event, const Pinned& pin) {
    const auto keys = split_path(pin.path);
    json* node = &event;
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        if (!node->is_object() || !node->contains(keys[i])) {
            return;
        }
        node = &(*node)[keys[i]];
    }
    if (!node->is_object()) {
        return;
    }
    if (node->contains(keys.back())) {
        (*node)[keys.back()] = pinned_value((*node)[keys.back()], pin.value);
    } else if (pin.insert_if_absent) {
        (*node)[keys.back()] = pin.value;
    }
}

json normalized(const std::string& name, json event) {
    for (const auto& pin : pinned_values()) {
        if (std::string(pin.event) == "*" || name == pin.event) {
            apply_pin(event, pin);
        }
    }
    return event;
}

} // namespace

TEST_CASE_METHOD(TelemetryTestFixture, "Contract: every event matches its golden",
                 "[telemetry][contract]") {
    lv_init_safe();
    auto& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    state.set_printer_type_sync("Voron 2.4");
    helix::ui::UpdateQueue::instance().drain_until_empty();

    ContractClient client;
    ScopedMoonrakerClient installed_client(&client);
    MoonrakerAPI api(client, state);
    api.hardware() = contract_printer();
    ScopedMoonrakerApi installed_api(&api);
    ScopedContractDisplay display;

    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    tm.clear_queue();
    const auto events = record_one_of_every_event(tm, *this);
    REQUIRE(events.size() == 17);

    const bool update = std::getenv("HELIX_UPDATE_GOLDENS") != nullptr;
    for (const auto& [name, raw] : events) {
        INFO("event: " << name);
        const json actual = normalized(name, raw);
        const std::string path = golden_dir() + name + ".json";
        if (update) {
            std::ofstream(path) << actual.dump(2) << "\n";
            WARN("golden rewritten: " << path);
            continue;
        }
        std::ifstream f(path);
        REQUIRE(f.good());
        const json expected = json::parse(f, nullptr, false);
        REQUIRE_FALSE(expected.is_discarded());
        // A JSON patch names each differing path, where comparing whole
        // objects would print two large blobs.
        const json diff = json::diff(expected, actual);
        INFO("golden -> actual patch: " << diff.dump());
        CHECK(diff.empty());
    }
}

TEST_CASE("Contract: every pinned path names a field some event carries", "[telemetry][contract]") {
    // A pin whose field was renamed would silently stop pinning anything; the
    // goldens are the record of what the events carry.
    for (const auto& pin : pinned_values()) {
        INFO("pin: " << pin.event << " " << pin.path);
        bool found = false;
        for (const auto& entry : std::filesystem::directory_iterator(golden_dir())) {
            const std::string event = entry.path().stem().string();
            if (std::string(pin.event) != "*" && event != pin.event) {
                continue;
            }
            std::ifstream f(entry.path());
            json golden = json::parse(f, nullptr, false);
            json* node = &golden;
            for (const auto& key : split_path(pin.path)) {
                node = node->is_object() && node->contains(key) ? &(*node)[key] : nullptr;
                if (!node) {
                    break;
                }
            }
            found = found || node != nullptr;
        }
        CHECK(found);
    }
}
