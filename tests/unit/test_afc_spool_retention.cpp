// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// AFC's remember_spool decides what a lane keeps through an eject, and a
// meter's flush writes AFC's weight and nothing else.

#include "../lvgl_test_fixture.h"
#include "ams_backend_afc.h"
#include "ams_types.h"
#include "lane_source_store.h"
#include "settings_manager.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/seeded_override.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace helix {

class AfcRetentionHelper : public AmsBackendAfc {
  public:
    AfcRetentionHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1", "lane2"};
        AfcTestAccess::initialize_slots(*this, names);
    }

    void set_override(int slot_index, const helix::ams::FilamentSlotOverride& o) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            AfcTestAccess::overrides(*this)[slot_index] = o;
        }
        helix::test::file_override_as_lane_records(*this, slot_index, o);
    }

    void feed_stepper(const std::string& lane, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_stepper " + lane] = data;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    bool has_gcode(const std::string& expected) const {
        return std::find(captured_gcodes.begin(), captured_gcodes.end(), expected) !=
               captured_gcodes.end();
    }

    bool has_gcode_starting_with(const std::string& prefix) const {
        return std::any_of(captured_gcodes.begin(), captured_gcodes.end(),
                           [&](const std::string& g) { return g.rfind(prefix, 0) == 0; });
    }

    [[nodiscard]] bool has_override(int slot_index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return AfcTestAccess::overrides(*this).count(slot_index) > 0;
    }

    std::vector<std::string> captured_gcodes;
};

} // namespace helix

using helix::AfcRetentionHelper;

namespace {

helix::ams::FilamentSlotOverride manual_record(const std::string& material) {
    helix::ams::FilamentSlotOverride o;
    o.material = material;
    o.brand = "Generic";
    return o;
}

} // namespace

TEST_CASE_METHOD(
    LVGLTestFixture,
    "an AFC lane with remember_spool off forgets its record and Spoolman link on eject",
    "[ams][afc][remember_spool]") {
    helix::SettingsManager::instance().init_subjects();
    helix::test::RegisteredBackend<AfcRetentionHelper> afc_reg;
    AfcRetentionHelper& afc = *afc_reg;

    afc.set_override(1, manual_record("ASA"));
    afc.feed_stepper(
        "lane2", nlohmann::json{{"status", "Loaded"}, {"remember_spool", false}, {"spool_id", 42}});
    helix::ams::Observation spoolman(helix::ams::ObservationSource::Spoolman);
    spoolman.spoolman_id = 42;
    spoolman.brand = "Polymaker";
    helix::ams::ingest(afc_reg.lane(1), spoolman);
    REQUIRE(afc.get_slot_info(1).material == "ASA");
    REQUIRE(helix::ams::lane_sources(afc_reg.lane(1)).record.has_value());

    // AFC's eject frame clears the lane's values along with its spool.
    afc.feed_stepper("lane2",
                     nlohmann::json{{"status", "None"}, {"spool_id", nullptr}, {"material", ""}});

    CHECK_FALSE(afc.has_override(1));
    const auto sources = helix::ams::lane_sources(afc_reg.lane(1));
    CHECK_FALSE(sources.record.has_value());
    CHECK_FALSE(sources.spoolman.has_value());
    CHECK(afc.get_slot_info(1).material.empty());
    CHECK(afc.get_slot_info(1).spoolman_id == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "an AFC lane with remember_spool on keeps its record on eject",
                 "[ams][afc][remember_spool]") {
    helix::SettingsManager::instance().init_subjects();
    helix::test::RegisteredBackend<AfcRetentionHelper> afc_reg;
    AfcRetentionHelper& afc = *afc_reg;

    afc.set_override(1, manual_record("ASA"));
    afc.feed_stepper("lane2", nlohmann::json{{"status", "Loaded"}, {"remember_spool", true}});
    REQUIRE(afc.get_slot_info(1).material == "ASA");

    afc.feed_stepper("lane2", nlohmann::json{{"status", "None"}});
    CHECK(afc.has_override(1));
    CHECK(helix::ams::lane_sources(afc_reg.lane(1)).record.has_value());
    CHECK(afc.get_slot_info(1).material == "ASA");
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC owns spool retention once every lane reports it",
                 "[ams][afc][remember_spool]") {
    helix::test::RegisteredBackend<AfcRetentionHelper> afc_reg;
    AfcRetentionHelper& afc = *afc_reg;

    CHECK_FALSE(afc.firmware_owns_spool_retention());
    afc.feed_stepper("lane1", nlohmann::json{{"remember_spool", false}});
    CHECK_FALSE(afc.firmware_owns_spool_retention());
    afc.feed_stepper("lane2", nlohmann::json{{"remember_spool", true}});
    CHECK(afc.firmware_owns_spool_retention());

    REQUIRE(afc.set_firmware_spool_retention(true).success());
    CHECK(afc.has_gcode("AFC_SET_REMEMBER_SPOOL LANE=lane1 REMEMBER_SPOOL=1"));
    CHECK(afc.has_gcode("AFC_SET_REMEMBER_SPOOL LANE=lane2 REMEMBER_SPOOL=1"));
    afc.captured_gcodes.clear();
    REQUIRE(afc.set_firmware_spool_retention(false).success());
    CHECK(afc.has_gcode("AFC_SET_REMEMBER_SPOOL LANE=lane1 REMEMBER_SPOOL=0"));
    CHECK(afc.has_gcode("AFC_SET_REMEMBER_SPOOL LANE=lane2 REMEMBER_SPOOL=0"));
}

TEST_CASE_METHOD(LVGLTestFixture, "an AFC weight flush writes the weight and no identity",
                 "[ams][afc][persistence]") {
    helix::test::RegisteredBackend<AfcRetentionHelper> afc_reg;
    AfcRetentionHelper& afc = *afc_reg;
    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"}});
    afc.captured_gcodes.clear();

    afc.update_slot_weight(0, 730.0f, 1000.0f, /*persist=*/true);

    CHECK(afc.has_gcode("SET_WEIGHT LANE=lane1 WEIGHT=730"));
    CHECK_FALSE(afc.has_gcode_starting_with("SET_COLOR"));
    CHECK_FALSE(afc.has_gcode_starting_with("SET_MATERIAL"));
    CHECK_FALSE(afc.has_gcode_starting_with("SET_SPOOL_ID"));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "an AFC unlink is AFC's empty SET_SPOOL_ID, and AFC's id is shown",
                 "[ams][afc][1717]") {
    helix::test::RegisteredBackend<AfcRetentionHelper> afc_reg;
    AfcRetentionHelper& afc = *afc_reg;
    afc.feed_stepper(
        "lane1", nlohmann::json{{"status", "Loaded"}, {"remember_spool", true}, {"spool_id", 42}});
    REQUIRE(afc.get_slot_info(0).spoolman_id == 42);

    const helix::SlotInfo original = afc.get_slot_info(0);
    helix::SlotInfo unlinked = original;
    unlinked.spoolman_id = 0;
    (void)afc.commit_user_edit(0, original, unlinked);
    CHECK(afc.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID="));

    // A remember_spool lane keeps its id through the unlink; once the
    // unlink's wait ends, the lane shows what AFC holds.
    helix::ams::withdraw_pending(afc_reg.lane(0));
    afc.feed_stepper("lane1", nlohmann::json{{"spool_id", 42}});
    CHECK(afc.get_slot_info(0).spoolman_id == 42);
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's spool vendor reads as a value, a clear or nothing",
                 "[ams][afc]") {
    helix::test::RegisteredBackend<AfcRetentionHelper> afc_reg;
    AfcRetentionHelper& afc = *afc_reg;
    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"}, {"spool_vendor", "Sunlu"}});
    REQUIRE(afc.get_slot_info(0).brand == "Sunlu");

    afc.feed_stepper("lane1", nlohmann::json{{"weight", 500}});
    CHECK(afc.get_slot_info(0).brand == "Sunlu");

    afc.feed_stepper("lane1", nlohmann::json{{"spool_vendor", ""}});
    CHECK(afc.get_slot_info(0).brand.empty());
    CHECK_FALSE(helix::ams::lane_sources(afc_reg.lane(0)).firmware->brand.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a link from outside the editor is written to AFC",
                 "[ams][afc][spoolman]") {
    helix::test::RegisteredBackend<AfcRetentionHelper> afc_reg;
    AfcRetentionHelper& afc = *afc_reg;
    afc.feed_stepper("lane1", nlohmann::json{{"status", "Loaded"}});
    helix::SlotInfo linked = afc.get_slot_info(0);
    linked.spoolman_id = 77;
    REQUIRE(afc.sync_external_identity(0, linked).success());
    CHECK(afc.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=77"));
}
