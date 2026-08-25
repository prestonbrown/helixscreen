// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: asserts against the text of src/system/moonraker_performance_source.cpp
//                 read from disk at run time, not against a copy of it -- there is nothing
//                 here that can drift away from production. It deliberately links no
//                 symbol: the guard is that the shipped file must NOT contain a
//                 printer.objects.subscribe call.

/**
 * @file test_moonraker_performance_source_regression.cpp
 * @brief Source-level regression guards for the v0.99.68 subscription wipe.
 *
 * Bug: MoonrakerPerformanceSource::rediscover_mcus() called
 * printer.objects.subscribe with only MCU objects. Moonraker docs:
 *
 *   "A new request will override a previous request."
 *
 * So that call wiped the discovery-sequence subscription (heaters, fans,
 * print_stats, virtual_sdcard, AFC, …) — every notify_status_update for the
 * core UI stopped arriving, leaving the screen frozen on the last known
 * values. User-visible symptom: "doesn't change heater target and if you
 * start a print", "hanging in updating values".
 *
 * Fix: MCU objects ride the union subscription built by
 * MoonrakerDiscoverySequence::build_subscription_objects(). PerformanceSource
 * only registers a notify_status_update handler — never subscribes.
 *
 * These tests assert at the source level so reviewers can't accidentally
 * re-introduce the bug without tripping a clearly-named failure. The
 * field-coverage half of the contract is in
 * test_moonraker_subscription_fields.cpp (MCU section).
 */

#include <fstream>
#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("MoonrakerPerformanceSource never issues printer.objects.subscribe",
          "[moonraker][performance][regression]") {
    // Path is relative to the CMake / Make working dir (project root).
    const std::string src = read_file("src/system/moonraker_performance_source.cpp");

    // Search for the C++ string literal "printer.objects.subscribe" — the
    // form an actual send_jsonrpc call would take. Quotes exclude prose in
    // comments (which legitimately reference the RPC name to explain the
    // guard). If you really need to mention the RPC name in a quoted form
    // inside a comment, route through a constexpr name token, not a literal.
    INFO("If this fails, MoonrakerPerformanceSource is about to call the "
         "subscribe RPC again — that REPLACES the main subscription per "
         "Moonraker docs and reproduces the v0.99.68 outage. MCU objects "
         "must ride the discovery-sequence union subscription instead.");
    REQUIRE(src.find("\"printer.objects.subscribe\"") == std::string::npos);
}

TEST_CASE("MoonrakerPerformanceSource hooks notify_status_update for MCU live + initial",
          "[moonraker][performance][regression]") {
    // After dropping its own subscribe call, PerformanceSource MUST hook the
    // notify stream to receive MCU updates that ride the discovery-sequence
    // union subscription. The hook MUST go through subscribe_notifications
    // (notify_callbacks_), NOT register_method_callback (method_callbacks_):
    // MoonrakerClient::dispatch_status_update fans out the initial-snapshot
    // delivery to notify_callbacks_ only. Routing through method_callbacks_
    // would silently drop the initial MCU rows until the first live frame.
    const std::string src = read_file("src/system/moonraker_performance_source.cpp");

    INFO("PerformanceSource must call subscribe_notifications (not "
         "register_method_callback) to receive both the initial subscription "
         "snapshot (delivered by dispatch_status_update through "
         "notify_callbacks_) and live frames. Dropping this hook freezes MCU "
         "rows; routing it through method_callbacks_ drops the initial state.");
    REQUIRE(src.find("subscribe_notifications") != std::string::npos);
}
