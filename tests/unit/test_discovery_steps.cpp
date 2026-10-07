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
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "discovery_steps.h"
#include "hardware_setup_prompter.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <algorithm>
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
