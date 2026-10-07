// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_discovery_steps.cpp
 * @brief The discovery-complete step table: its order, its hw_changed gating and the
 *        print_active decision every wizard and gcode gate in the pass shares.
 *
 * Later steps read what earlier ones stored (hardware before status dispatch, auto-detect
 * before validation, validation before the prompts), so the table is pinned by name. The
 * walker is exercised against a recording table: running the real steps needs a live
 * printer.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/breadcrumb_capture.h"
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "discovery_steps.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <string>
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
                                status, prompter,   nullptr,        nullptr,
                                n,      hw_changed, false};
    }
};

std::vector<std::string> step_names() {
    std::vector<std::string> names;
    for (const DiscoveryStep& step : helix::discovery_steps()) {
        names.emplace_back(step.name);
    }
    return names;
}

} // namespace

TEST_CASE("the discovery steps run in this order", "[discovery_steps]") {
    // Hardware lands in PrinterState before the status replay and the fan roles it
    // reads; auto-detect and the heater heal precede validation so the validator sees
    // post-preset roles; the prompts come after validation and before its snapshot is
    // saved.
    const std::vector<std::string> expected = {
        "update_checker_connected",
        "set_hardware",
        "zoffset_persistence",
        "temp_graph_seed",
        "status_dispatch",
        "software_versions",
        "about_print_hours",
        "timelapse_events",
        "power_sensor_subscribe",
        "auto_detect_printer",
        "heal_heater_roles",
        "acknowledge_deferred_hardware",
        "validate_hardware",
        "hardware_prompts",
        "save_validation_snapshot",
        "telemetry",
        "safety_limits",
        "helix_plugin_check",
        "spoolman_sync",
        "job_queue_fetch",
        "settle_light_buttons",
        "auto_update_check",
        "moonraker_update_channel",
        "manual_probe_autoopen",
    };
    CHECK(step_names() == expected);
}

TEST_CASE("only the steps that are pure functions of the hardware shape are gated",
          "[discovery_steps]") {
    std::vector<std::string> gated;
    for (const DiscoveryStep& step : helix::discovery_steps()) {
        CHECK(step.run != nullptr);
        if (step.only_when_hw_changed) {
            gated.emplace_back(step.name);
        }
    }
    CHECK(gated == std::vector<std::string>{"auto_detect_printer", "heal_heater_roles"});
}

TEST_CASE("the discovery breadcrumbs keep their keys", "[discovery_steps]") {
    std::vector<std::string> crumbs;
    for (const DiscoveryStep& step : helix::discovery_steps()) {
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
