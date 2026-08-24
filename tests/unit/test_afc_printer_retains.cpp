// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_printer_retains.cpp
 * @brief printer_retains_spool_info(): is AFC's own per-lane
 * remember_spool retention currently owning eject behavior? (#1281
 * follow-up)
 *
 * When every lane reports remember_spool = true, AFC itself repopulates
 * lanes on eject, so "Keep Spool Info on Eject" has no observable effect
 * either way: firmware keeps reporting the spool id, so neither the
 * merge's eject rule nor the #1289 re-assert push ever fires. The AMS
 * Management overlay shows the toggle disabled with a note instead of
 * letting it silently lie. ALL semantics: any lane at false still clears
 * on eject, so the toggle keeps governing those lanes and stays enabled.
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_afc.h"
#include "ams_types.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

// Global scope (not anonymous) to match the friend declaration in
// include/ams_backend_afc.h — same convention as AfcReassertHelper.
class AfcRetainsHelper : public AmsBackendAfc {
  public:
    AfcRetainsHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1", "lane2"};
        initialize_slots(names);
    }

    /// Drive the live status path — the same feed the parse uses in
    /// production, so assertions see what lane_remember_spool_ really holds.
    void feed_stepper(const std::string& lane, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_stepper " + lane] = data;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }
};

TEST_CASE_METHOD(LVGLTestFixture,
                 "AFC printer_retains_spool_info requires every lane at remember_spool true",
                 "[ams][afc][spool-retention]") {
    AfcRetainsHelper afc;

    // Nothing reported yet — the everyday default is not retaining.
    CHECK_FALSE(afc.printer_retains_spool_info());

    // AFC default (remember_spool = false): the lane clears on eject and
    // the HelixScreen toggle governs retention.
    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"}, {"remember_spool", false}});
    CHECK_FALSE(afc.printer_retains_spool_info());

    // Every lane retaining: firmware owns retention end to end.
    afc.feed_stepper("lane1", nlohmann::json{{"remember_spool", true}});
    afc.feed_stepper("lane2", nlohmann::json{{"status", "Loaded"}, {"remember_spool", true}});
    CHECK(afc.printer_retains_spool_info());

    // Mixed config: the toggle still governs the remember_spool = false
    // lanes, so it must stay enabled.
    afc.feed_stepper("lane2", nlohmann::json{{"remember_spool", false}});
    CHECK_FALSE(afc.printer_retains_spool_info());

    // Moonraker sends deltas: a frame without the key keeps the last value.
    afc.feed_stepper("lane2", nlohmann::json{{"remember_spool", true}});
    afc.feed_stepper("lane2", nlohmann::json{{"weight", 750.0}});
    CHECK(afc.printer_retains_spool_info());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "AFC lanes silent on remember_spool are conservatively not retaining",
                 "[ams][afc][spool-retention]") {
    AfcRetainsHelper afc;

    // Only one of two lanes ever reported the key. Treating silent lanes as
    // retaining would disable the toggle on partial information; the safe
    // default keeps it enabled.
    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"}, {"remember_spool", true}});
    CHECK_FALSE(afc.printer_retains_spool_info());
}

TEST_CASE_METHOD(LVGLTestFixture, "printer_retains_spool_info base default is false",
                 "[ams][capabilities][spool-retention]") {
    // Qualified call pins the BASE default (false), matching the
    // printer_reports_spool_ids pattern in test_ams_firmware_persistence.cpp.
    auto afc = std::make_unique<AmsBackendAfc>(nullptr, nullptr);
    CHECK_FALSE(afc->AmsBackend::printer_retains_spool_info());
}
