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
#include "test_helpers/afc_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/seeded_override.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace helix {

class AfcReassertHelper : public AmsBackendAfc {
  public:
    AfcReassertHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1", "lane2"};
        AfcTestAccess::initialize_slots(*this, names);
    }

    void set_override(int slot_index, const helix::ams::FilamentSlotOverride& o) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            AfcTestAccess::overrides(*this)[slot_index] = o;
        }
        // The backend's own init files both stores together.
        helix::test::file_override_as_lane_records(*this, slot_index, o);
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

    [[nodiscard]] std::string visible_material(int slot_index) const {
        return get_slot_info(slot_index).material;
    }

    /// File what Spoolman says about a lane's spool, as SpoolmanManager does.
    void ingest_spoolman(int slot_index, int spool_id, const std::string& material,
                         const std::string& brand) {
        helix::ams::Observation spoolman(helix::ams::ObservationSource::Spoolman);
        spoolman.spoolman_id = spool_id;
        spoolman.material = material;
        spoolman.brand = brand;
        helix::ams::ingest(lane_id(slot_index), spoolman);
    }

    [[nodiscard]] bool has_override(int slot_index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return AfcTestAccess::overrides(*this).count(slot_index) > 0;
    }

    /// Consult the pending own-write expectation under mutex_ (as
    /// apply_overrides does) — proves the push rode the echo-expectation
    /// plumbing rather than firing bare.
    [[nodiscard]] std::pair<int, int> peek_expectation(int slot_index, int firmware_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return own_write_expectation(slot_index, firmware_id);
    }

    /// Overwrite the global_index copy on a lane's SlotInfo, so a test can tell
    /// a store keyed by registry position from one keyed by that field.
    void stamp_global_index(int slot_index, int global_index) {
        std::lock_guard<std::mutex> lock(mutex_);
        AfcTestAccess::slots(*this).get_mut(slot_index)->info.global_index = global_index;
    }

    std::vector<std::string> captured_gcodes;
};
} // namespace helix

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

    helix::test::RegisteredBackend<AfcReassertHelper> afc_reg;
    AfcReassertHelper& afc = *afc_reg;
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

    helix::test::RegisteredBackend<AfcReassertHelper> afc_reg;
    AfcReassertHelper& afc = *afc_reg;
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

    helix::test::RegisteredBackend<AfcReassertHelper> afc_reg;
    AfcReassertHelper& afc = *afc_reg;
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

    helix::test::RegisteredBackend<AfcReassertHelper> afc_reg;
    AfcReassertHelper& afc = *afc_reg;
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

TEST_CASE_METHOD(LVGLTestFixture,
                 "AFC re-assert keys the lane by registry position, not SlotInfo::global_index "
                 "(#1644)",
                 "[1644][ams][afc]") {
    SettingsManager::instance().init_subjects();
    SettingsManager::instance().set_ams_keep_spool_info_on_eject(true);

    helix::test::RegisteredBackend<AfcReassertHelper> afc_reg;
    AfcReassertHelper& afc = *afc_reg;
    afc.set_override(1, spool_override(42));
    // The registry stamps global_index equal to the lane's position, so the two
    // keys only come apart when the field is forced; every store must follow
    // the position regardless.
    afc.stamp_global_index(1, 7);

    afc.feed_stepper("lane2", nlohmann::json{{"status", "None"}, {"spool_id", nullptr}});
    afc.feed_stepper("lane2", nlohmann::json{{"status", "Loaded"}});
    CHECK(afc.gcode_count("SET_SPOOL_ID LANE=lane2 SPOOL_ID=42") == 1);
    CHECK(afc.peek_expectation(1, 0) == std::make_pair(0, 42));

    // A frame still naming the pre-push id (none) holds the binding and keeps
    // the expectation pending.
    afc.feed_stepper("lane2", nlohmann::json{{"status", "Loaded"}, {"spool_id", nullptr}});
    CHECK(afc.has_override(1));
    CHECK(afc.visible_spool_id(1) == 42);
    CHECK(afc.peek_expectation(1, 0) == std::make_pair(0, 42));

    // The echo lands under the same key the push was recorded under.
    afc.feed_stepper("lane2", nlohmann::json{{"spool_id", 42}});
    CHECK(afc.has_override(1));
    CHECK(afc.visible_spool_id(1) == 42);
    CHECK(afc.peek_expectation(1, 42) == std::make_pair(0, 0));

    // Another writer re-binds the lane: the break clears the override filed
    // under the same key.
    afc.feed_stepper("lane2", nlohmann::json{{"spool_id", 99}});
    CHECK_FALSE(afc.has_override(1));
}

// AFC publishes its own per-lane setting for what an eject does to a lane's
// spool details, remember_spool. Where a lane reports it, AFC decides, and
// HelixScreen's own "Keep Spool Info on Eject" setting does not apply.

TEST_CASE_METHOD(LVGLTestFixture,
                 "AFC lane with remember_spool off forgets a Spoolman link on eject even with "
                 "Keep Spool Info on",
                 "[ams][afc][remember_spool]") {
    SettingsManager::instance().init_subjects();
    SettingsManager::instance().set_ams_keep_spool_info_on_eject(true);

    helix::test::RegisteredBackend<AfcReassertHelper> afc_reg;
    AfcReassertHelper& afc = *afc_reg;

    // A lane AFC itself linked (Mainsail or a macro): no HelixScreen record,
    // only what Spoolman says about the spool firmware names.
    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"},
                                             {"remember_spool", false},
                                             {"spool_id", 127},
                                             {"material", "ASA"}});
    afc.ingest_spoolman(0, 127, "ASA", "Overture");
    afc.feed_stepper("lane1", nlohmann::json{{"weight", 430.0}}); // the next frame paints it
    REQUIRE(afc.visible_spool_id(0) == 127);
    REQUIRE(afc.visible_material(0) == "ASA");

    // Eject: AFC clears the lane, and so does HelixScreen.
    afc.feed_stepper("lane1",
                     nlohmann::json{{"status", "None"}, {"spool_id", nullptr}, {"material", ""}});
    CHECK(afc.visible_spool_id(0) == 0);
    CHECK(afc.visible_material(0).empty());

    // Reload: AFC fills its default material; HelixScreen shows what AFC
    // says and pushes nothing back.
    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"}, {"material", "ABS"}});
    CHECK(afc.visible_spool_id(0) == 0);
    CHECK(afc.visible_material(0) == "ABS");
    CHECK_FALSE(afc.has_gcode_starting_with("SET_SPOOL_ID"));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "AFC lane with remember_spool off forgets HelixScreen's own record on eject",
                 "[ams][afc][remember_spool]") {
    SettingsManager::instance().init_subjects();
    SettingsManager::instance().set_ams_keep_spool_info_on_eject(true);

    helix::test::RegisteredBackend<AfcReassertHelper> afc_reg;
    AfcReassertHelper& afc = *afc_reg;

    // Lane 1 linked in HelixScreen's editor; lane 2 a manual entry with no spool.
    afc.set_override(0, spool_override(42));
    helix::ams::FilamentSlotOverride manual;
    manual.material = "ASA";
    manual.brand = "Generic";
    afc.set_override(1, manual);
    afc.feed_stepper(
        "lane1", nlohmann::json{{"status", "Loaded"}, {"remember_spool", false}, {"spool_id", 42}});
    afc.feed_stepper("lane2", nlohmann::json{{"status", "Loaded"}, {"remember_spool", false}});
    REQUIRE(afc.visible_spool_id(0) == 42);
    REQUIRE(afc.visible_material(1) == "ASA");

    afc.feed_stepper("lane1", nlohmann::json{{"status", "None"}, {"spool_id", nullptr}});
    afc.feed_stepper("lane2", nlohmann::json{{"status", "None"}, {"material", ""}});
    CHECK_FALSE(afc.has_override(0));
    CHECK_FALSE(afc.has_override(1));
    CHECK(afc.visible_spool_id(0) == 0);
    CHECK(afc.visible_material(1).empty());

    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"}});
    afc.feed_stepper("lane2", nlohmann::json{{"status", "Loaded"}});
    CHECK_FALSE(afc.has_gcode_starting_with("SET_SPOOL_ID"));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "AFC lane with remember_spool on keeps its details through an eject",
                 "[ams][afc][remember_spool]") {
    // Keep Spool Info off: AFC's own setting still wins.
    SettingsManager::instance().init_subjects();
    SettingsManager::instance().set_ams_keep_spool_info_on_eject(false);

    helix::test::RegisteredBackend<AfcReassertHelper> afc_reg;
    AfcReassertHelper& afc = *afc_reg;
    helix::ams::FilamentSlotOverride manual;
    manual.material = "ASA";
    afc.set_override(1, manual);
    afc.feed_stepper("lane2", nlohmann::json{{"status", "Loaded"}, {"remember_spool", true}});
    REQUIRE(afc.visible_material(1) == "ASA");

    // AFC keeps the lane's values, so its eject frame names no clear.
    afc.feed_stepper("lane2", nlohmann::json{{"status", "None"}});
    CHECK(afc.has_override(1));
    CHECK(afc.visible_material(1) == "ASA");
    SettingsManager::instance().set_ams_keep_spool_info_on_eject(true);
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC spool retention is AFC's setting once every lane reports it",
                 "[ams][afc][remember_spool]") {
    helix::test::RegisteredBackend<AfcReassertHelper> afc_reg;
    AfcReassertHelper& afc = *afc_reg;

    // Older AFC reports nothing: HelixScreen's setting governs.
    CHECK_FALSE(afc.firmware_owns_spool_retention());
    afc.feed_stepper("lane1", nlohmann::json{{"remember_spool", false}});
    CHECK_FALSE(afc.firmware_owns_spool_retention());
    afc.feed_stepper("lane2", nlohmann::json{{"remember_spool", true}});
    CHECK(afc.firmware_owns_spool_retention());

    // The switch writes AFC's setting on every lane.
    REQUIRE(afc.set_firmware_spool_retention(true).success());
    CHECK(afc.has_gcode("AFC_SET_REMEMBER_SPOOL LANE=lane1 REMEMBER_SPOOL=1"));
    CHECK(afc.has_gcode("AFC_SET_REMEMBER_SPOOL LANE=lane2 REMEMBER_SPOOL=1"));
    afc.captured_gcodes.clear();
    REQUIRE(afc.set_firmware_spool_retention(false).success());
    CHECK(afc.has_gcode("AFC_SET_REMEMBER_SPOOL LANE=lane1 REMEMBER_SPOOL=0"));
    CHECK(afc.has_gcode("AFC_SET_REMEMBER_SPOOL LANE=lane2 REMEMBER_SPOOL=0"));
}
