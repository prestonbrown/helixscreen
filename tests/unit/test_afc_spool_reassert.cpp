// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_spool_reassert.cpp
 * @brief Re-assert a retained Spoolman binding into AFC when a lane
 * transitions empty -> loaded and firmware reports no spool id (#1289).
 *
 * With "Keep Spool Info on Eject" on, HelixScreen keeps a lane's spool
 * identity in its private override namespace while AFC (default
 * remember_spool = false) clears the lane. Re-inserting the same spool then
 * shows the retained identity here but "unknown" in Mainsail. On the
 * empty -> loaded edge — and only there — the backend pushes the retained id
 * back into AFC via SET_SPOOL_ID, riding the same own-write echo
 * expectation the editor's re-link uses.
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_afc.h"
#include "ams_types.h"
#include "settings_manager.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

class AfcReassertHelper : public AmsBackendAfc {
  public:
    AfcReassertHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1", "lane2"};
        initialize_slots(names);
    }

    void set_override(int slot_index, const helix::ams::FilamentSlotOverride& o) {
        std::lock_guard<std::mutex> lock(mutex_);
        overrides_[slot_index] = o;
    }

    /// Drive the live status path, so assertions see what the UI would paint.
    void feed_stepper(const std::string& lane, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_stepper " + lane] = data;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    // The same gcode capture AmsBackendAfcTestHelper uses for persistence
    // tests: api_ is null here, so override the dispatch point instead.
    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    bool has_gcode(const std::string& expected) const {
        return std::find(captured_gcodes.begin(), captured_gcodes.end(), expected) !=
               captured_gcodes.end();
    }

    int gcode_count(const std::string& expected) const {
        return static_cast<int>(
            std::count(captured_gcodes.begin(), captured_gcodes.end(), expected));
    }

    bool has_gcode_starting_with(const std::string& prefix) const {
        for (const auto& gcode : captured_gcodes) {
            if (gcode.rfind(prefix, 0) == 0)
                return true;
        }
        return false;
    }

    [[nodiscard]] int visible_spool_id(int slot_index) const {
        return get_slot_info(slot_index).spoolman_id;
    }

    [[nodiscard]] bool has_override(int slot_index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return overrides_.count(slot_index) > 0;
    }

    /// Consult the pending own-write expectation under mutex_ (as
    /// apply_overrides does) — proves the push rode the echo-expectation
    /// plumbing rather than firing bare.
    [[nodiscard]] std::pair<int, int> peek_expectation(int slot_index, int firmware_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return own_write_expectation(slot_index, firmware_id);
    }

    std::vector<std::string> captured_gcodes;
};

namespace {

helix::ams::FilamentSlotOverride spool_override(int spoolman_id) {
    helix::ams::FilamentSlotOverride o;
    o.spoolman_id = spoolman_id;
    o.brand = "Polymaker";
    o.material = "PLA";
    return o;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AFC re-asserts retained binding on empty->loaded (#1289)",
                 "[1289][ams][afc]") {
    SettingsManager::instance().init_subjects();
    SettingsManager::instance().set_ams_keep_spool_info_on_eject(true);

    AfcReassertHelper afc;
    afc.set_override(0, spool_override(42));

    // Spool present and linked: firmware echoes our id. Even though a rising
    // edge (UNKNOWN -> loaded) happens here, firmware already knows the id,
    // so nothing must be pushed.
    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"}, {"spool_id", 42}});
    CHECK(afc.visible_spool_id(0) == 42);
    CHECK_FALSE(afc.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=42"));

    // Eject: AFC (remember_spool = false) drops the link — spool_id None —
    // and the lane empties. Retention keeps our record painting.
    afc.feed_stepper("lane1", nlohmann::json{{"status", "None"}, {"spool_id", nullptr}});
    CHECK(afc.visible_spool_id(0) == 42);
    CHECK(afc.has_override(0));
    CHECK_FALSE(afc.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=42"));

    // Re-insert the same spool: lane loads, firmware reports no id. The
    // binding must be pushed back into AFC exactly once.
    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"}});
    CHECK(afc.gcode_count("SET_SPOOL_ID LANE=lane1 SPOOL_ID=42") == 1);
    CHECK(afc.has_override(0)); // the push is a re-assert, not a clear

    // The push must ride the own-write echo expectation: a pending {0, 42}
    // entry exists until firmware echoes the id.
    CHECK(afc.peek_expectation(0, 0) == std::make_pair(0, 42));

    // Firmware echoes OUR push. The expectation is consumed and the override
    // survives — HelixScreen, AFC and Mainsail now agree on 42.
    afc.feed_stepper("lane1", nlohmann::json{{"spool_id", 42}});
    CHECK(afc.has_override(0));
    CHECK(afc.visible_spool_id(0) == 42);
    CHECK(afc.peek_expectation(0, 42) == std::make_pair(0, 0));
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC re-assert skipped when firmware reports an id (#1289)",
                 "[1289][ams][afc]") {
    // remember_spool = true users: AFC repopulated the lane itself on insert.
    // The merge policy handles it; no SET_SPOOL_ID may be sent.
    SettingsManager::instance().init_subjects();
    SettingsManager::instance().set_ams_keep_spool_info_on_eject(true);

    AfcReassertHelper afc;
    afc.set_override(0, spool_override(42));

    afc.feed_stepper("lane1", nlohmann::json{{"status", "None"}, {"spool_id", nullptr}});
    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"}, {"spool_id", 42}});
    CHECK_FALSE(afc.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=42"));
    CHECK(afc.has_override(0));
    CHECK(afc.visible_spool_id(0) == 42);
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC re-assert never fires with retention off (#1289)",
                 "[1289][ams][afc]") {
    auto& settings = SettingsManager::instance();
    settings.init_subjects();
    settings.set_ams_keep_spool_info_on_eject(false);

    AfcReassertHelper afc;
    // Retention off holds by two overlapping locks: the merge's eject rule
    // (Rule 2) drops the in-memory record on the next frame after an eject
    // clear, and the re-assert gate independently refuses to push while the
    // setting is off. Pin the lingering-record shape anyway — a record that
    // lands after the clear (e.g. a late store load) must never be pushed.
    afc.feed_stepper("lane1", nlohmann::json{{"status", "None"}, {"spool_id", nullptr}});
    afc.set_override(0, spool_override(42));
    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"}});
    CHECK_FALSE(afc.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=42"));

    // An override with no Spoolman id has nothing to re-assert either.
    afc.set_override(1, spool_override(0));
    afc.feed_stepper("lane2", nlohmann::json{{"status", "None"}, {"spool_id", nullptr}});
    afc.feed_stepper("lane2", nlohmann::json{{"status", "Loaded"}});
    CHECK_FALSE(afc.has_gcode_starting_with("SET_SPOOL_ID"));

    settings.set_ams_keep_spool_info_on_eject(true);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "AFC re-assert is edge-triggered: bounce sends one write per "
                 "transition, level frames send none (#1289)",
                 "[1289][ams][afc]") {
    SettingsManager::instance().init_subjects();
    SettingsManager::instance().set_ams_keep_spool_info_on_eject(true);

    AfcReassertHelper afc;
    afc.set_override(0, spool_override(42));

    // A spool bouncing in and out: two empty -> loaded transitions, at most
    // two writes. Frames while the lane stays loaded must not re-send, and
    // no retry loop runs if the write's echo never lands.
    afc.feed_stepper("lane1", nlohmann::json{{"status", "None"}, {"spool_id", nullptr}});
    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"}}); // transition 1
    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"}}); // level: no write
    afc.feed_stepper("lane1", nlohmann::json{{"weight", 900.0}});    // delta: no write
    afc.feed_stepper("lane1", nlohmann::json{{"status", "None"}});   // bounce out
    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"}}); // transition 2
    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"}}); // level: no write
    CHECK(afc.gcode_count("SET_SPOOL_ID LANE=lane1 SPOOL_ID=42") == 2);
}
