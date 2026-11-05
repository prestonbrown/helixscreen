// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whether the identity declared on a lane still describes what is in it.
// classify_binding() is pure and its cases need no store; reconcile_binding()
// and the backend call sites touch the process-wide lane store, so every case
// below that reaches it carries a fixture that resets it.

#include "../lvgl_test_fixture.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_error.h"
#include "ams_state.h"
#include "helix_test_fixture.h"
#include "lane_binding.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "settings_manager.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/happy_hare_test_access.h"
#include "test_helpers/registered_backend.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AfcTestAccess;
using helix::AmsBackendAfc;
using helix::AmsBackendHappyHare;
using helix::AmsError;
using helix::AmsErrorHelper;
using helix::CfsTestAccess;
using helix::HappyHareTestAccess;
using helix::SettingsManager;
using helix::SlotInfo;
using helix::ams::BindingReading;
using helix::ams::BindingVerdict;
using helix::ams::classify_binding;
using helix::ams::commit_slot_edit;
using helix::ams::drop_lane_source;
using helix::ams::ingest;
using helix::ams::lane_sources;
using helix::ams::LaneSources;
using helix::ams::Observation;
using helix::ams::ObservationSource;
using helix::ams::reconcile_binding;
using helix::ams::resolve;
using helix::printer::AmsBackendCfs;
using helix::test::RegisteredBackend;

namespace {

/// A lane whose identity the Spoolman server declares, plus the fields that
/// ride in with a link. The shape a linked lane actually holds.
LaneSources linked_to(int spool_id) {
    LaneSources sources;
    Observation server(ObservationSource::Spoolman);
    server.spoolman_id = spool_id;
    server.brand = "Polymaker";
    server.material = "PLA";
    sources.spoolman = server;
    return sources;
}

/// What a backend that names a spool id reports, with nothing else set.
BindingReading firmware_says(int spool_id) {
    BindingReading reading;
    reading.firmware_spool_id = spool_id;
    return reading;
}

/// The same, on a backend whose 0 means ejected rather than "no reading".
BindingReading id_reporting_firmware_says(int spool_id, bool keep_on_eject) {
    BindingReading reading = firmware_says(spool_id);
    reading.printer_reports_spool_ids = true;
    reading.keep_spool_info_on_eject = keep_on_eject;
    return reading;
}

using AfcHarness = RegisteredBackend<AmsBackendAfc>;
using HappyHareHarness = RegisteredBackend<AmsBackendHappyHare>;
using CfsHarness = helix::test::RegisteredWired<AmsBackendCfs>;

void init_afc_lanes(AmsBackendAfc& backend) {
    AfcTestAccess::initialize_slots(backend, std::vector<std::string>{"lane1", "lane2"});
}

/// One AFC_stepper lane object, through the envelope Moonraker delivers.
void feed_afc_lane(AmsBackendAfc& backend, const std::string& lane_name,
                   const nlohmann::json& data) {
    nlohmann::json params;
    params["AFC_stepper " + lane_name] = data;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    AfcTestAccess::handle_status_update(backend, notification);
}

/// One printer.mmu object, through the same envelope.
void feed_mmu(AmsBackendHappyHare& backend, const nlohmann::json& mmu) {
    nlohmann::json params;
    params["mmu"] = mmu;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    HappyHareTestAccess::handle_status_update(backend, notification);
}

/// A community-fork CFS `box`: a flat self-describing slots[] array, the one
/// CFS schema that states a per-bay spool id.
void feed_cfs_flat(helix::test::WiredBackend<AmsBackendCfs>& backend, int spoolman_id) {
    nlohmann::json slot{{"id", 0},           {"name", "PLA Black"},       {"material", "PLA"},
                        {"brand", "Elegoo"}, {"color", "ED2C2C"},         {"present", true},
                        {"loaded", false},   {"spoolman_id", spoolman_id}};
    nlohmann::json box{{"api_version", 1}, {"slots", nlohmann::json::array({slot})}};
    backend.fire_status({{"box", box}});
}

/// The user's own declaration on a lane: a link and nothing else, which is
/// exactly what user_edit_observation() files for a binding change.
void user_links(helix::ams::LaneId lane, int spool_id) {
    Observation user(ObservationSource::LocalUser);
    user.spoolman_id = spool_id;
    commit_slot_edit(lane, user);
}

/// An AFC backend whose gcode is captured rather than sent. api_ is null in
/// these cases, so the virtual dispatch point is where a save's writes can be
/// observed at all.
class GcodeCapturingAfc : public AmsBackendAfc {
  public:
    GcodeCapturingAfc() : AmsBackendAfc(nullptr, nullptr) {}

    AmsError execute_gcode(const std::string& gcode) override {
        sent_.push_back(gcode);
        return AmsErrorHelper::success();
    }

    [[nodiscard]] bool sent_gcode(const std::string& expected) const {
        return std::find(sent_.begin(), sent_.end(), expected) != sent_.end();
    }

  private:
    std::vector<std::string> sent_;
};

} // namespace

// ============================================================================
// classify_binding - pure, no store, no fixture
// ============================================================================

TEST_CASE("a binding holds while firmware agrees with the declared spool", "[lane][binding]") {
    CHECK(classify_binding(linked_to(42), firmware_says(42)) == BindingVerdict::Holds);
}

TEST_CASE("firmware naming a different spool breaks the declared binding", "[lane][binding]") {
    CHECK(classify_binding(linked_to(42), firmware_says(7)) == BindingVerdict::Rebound);
}

TEST_CASE("a re-bind ignores the retention setting, which is not about re-binds",
          "[lane][binding]") {
    BindingReading reading = firmware_says(7);
    reading.printer_reports_spool_ids = true;
    reading.keep_spool_info_on_eject = true;
    CHECK(classify_binding(linked_to(42), reading) == BindingVerdict::Rebound);

    reading.keep_spool_info_on_eject = false;
    CHECK(classify_binding(linked_to(42), reading) == BindingVerdict::Rebound);
}

TEST_CASE("a re-bind never fires on the eject signal", "[lane][binding]") {
    // 0 is the absence of a spool, not a spool numbered zero.
    CHECK(classify_binding(linked_to(42), firmware_says(0)) != BindingVerdict::Rebound);
    CHECK(classify_binding(linked_to(42), firmware_says(-1)) != BindingVerdict::Rebound);
}

TEST_CASE("a lane declaring no spool has no binding to break", "[lane][binding]") {
    // A person picked a colour and linked nothing. Firmware naming an id is
    // the lane acquiring a binding, not losing one.
    LaneSources sources;
    Observation user(ObservationSource::LocalUser);
    user.color_rgb = 0x00FF00u;
    sources.local_user = user;

    CHECK(classify_binding(sources, firmware_says(169)) == BindingVerdict::Holds);
    CHECK(classify_binding(sources, id_reporting_firmware_says(0, false)) == BindingVerdict::Holds);
}

TEST_CASE("a user's own link is a declared binding, like the server's", "[lane][binding]") {
    LaneSources sources;
    Observation user(ObservationSource::LocalUser);
    user.spoolman_id = 42;
    sources.local_user = user;

    CHECK(classify_binding(sources, firmware_says(42)) == BindingVerdict::Holds);
    CHECK(classify_binding(sources, firmware_says(169)) == BindingVerdict::Rebound);
}

TEST_CASE("firmware's own cache is not a declaration and is never compared against itself",
          "[lane][binding]") {
    // A lane nobody has declared anything about, carrying only what firmware
    // remembered last poll. The record refreshes on either side of the call
    // depending on which parser ran, so reading it here would make the verdict
    // depend on parse order and invalidate records that were never stale.
    LaneSources sources;
    Observation cache(ObservationSource::VendorCache);
    cache.spoolman_id = 42;
    cache.brand = "Elegoo";
    sources.vendor_cache = cache;

    CHECK(classify_binding(sources, firmware_says(7)) == BindingVerdict::Holds);
    CHECK(classify_binding(sources, id_reporting_firmware_says(0, false)) == BindingVerdict::Holds);

    // And a cache standing beside a real declaration does not shadow it: the
    // server's id is the one the binding is judged against.
    LaneSources with_server = sources;
    with_server.spoolman = *linked_to(169).spoolman;
    CHECK(classify_binding(with_server, firmware_says(169)) == BindingVerdict::Holds);
    CHECK(classify_binding(with_server, firmware_says(42)) == BindingVerdict::Rebound);
}

TEST_CASE("the server outranks the user when both declare a binding", "[lane][binding]") {
    // resolve()'s identity ladder puts Spoolman last, so its id is the
    // declared one; this must not drift from it.
    LaneSources sources = linked_to(42);
    Observation user(ObservationSource::LocalUser);
    user.spoolman_id = 169;
    sources.local_user = user;

    CHECK(resolve(sources).spoolman_id == 42);
    CHECK(classify_binding(sources, firmware_says(42)) == BindingVerdict::Holds);
    CHECK(classify_binding(sources, firmware_says(169)) == BindingVerdict::Rebound);
}

// --- own-write suppression --------------------------------------------------

TEST_CASE("a stale frame naming the id we overwrote is our own write, not a re-bind",
          "[lane][binding]") {
    // We re-linked 42 -> 169. In-flight frames keep reporting 42 for a poll
    // or two; reading one as another writer's statement would destroy the
    // declaration we just filed.
    BindingReading reading = firmware_says(42);
    reading.own_write_old_id = 42;
    reading.own_write_new_id = 169;
    CHECK(classify_binding(linked_to(169), reading) == BindingVerdict::Holds);
}

TEST_CASE("the echo of our own write is not a re-bind", "[lane][binding]") {
    BindingReading reading = firmware_says(169);
    reading.own_write_old_id = 42;
    reading.own_write_new_id = 169;
    CHECK(classify_binding(linked_to(169), reading) == BindingVerdict::Holds);
}

TEST_CASE("a third id during our own write is a genuine external change", "[lane][binding]") {
    BindingReading reading = firmware_says(200);
    reading.own_write_old_id = 42;
    reading.own_write_new_id = 169;
    CHECK(classify_binding(linked_to(169), reading) == BindingVerdict::Rebound);
}

// --- eject ------------------------------------------------------------------

TEST_CASE("retention on keeps a declared identity over an emptied lane", "[lane][binding]") {
    CHECK(classify_binding(linked_to(42), id_reporting_firmware_says(0, true)) ==
          BindingVerdict::Holds);
}

TEST_CASE("retention off starts an ejected lane fresh", "[lane][binding]") {
    const auto verdict = classify_binding(linked_to(42), id_reporting_firmware_says(0, false));
    CHECK(verdict == BindingVerdict::Ejected);
    CHECK(verdict != BindingVerdict::Rebound);
}

TEST_CASE("the eject signal is only a signal where firmware states ids", "[lane][binding]") {
    // Stock CFS, ACE, IFS, Snapmaker: 0 every poll is the everyday reading.
    // Clearing on it would empty every lane the user had assigned.
    BindingReading reading = firmware_says(0);
    reading.printer_reports_spool_ids = false;
    reading.keep_spool_info_on_eject = false;
    CHECK(classify_binding(linked_to(42), reading) == BindingVerdict::Holds);
}

TEST_CASE("an unlinked lane is never ejected, whatever the setting says", "[lane][binding]") {
    LaneSources sources;
    Observation user(ObservationSource::LocalUser);
    user.color_rgb = 0x00FF00u;
    user.material = "ABS";
    sources.local_user = user;

    CHECK(classify_binding(sources, id_reporting_firmware_says(0, false)) == BindingVerdict::Holds);
    // Proof the colour is still there to be painted rather than quietly gone.
    CHECK(resolve(sources).color_rgb == 0x00FF00u);
}

// ============================================================================
// reconcile_binding - reaches the process-wide store
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "a broken binding takes both declaring records with it",
                 "[lane][binding]") {
    constexpr helix::ams::LaneId kLane = helix::ams::lane_id_for(0, 3);

    Observation server(ObservationSource::Spoolman);
    server.spoolman_id = 42;
    server.brand = "Polymaker";
    ingest(kLane, server);

    Observation user(ObservationSource::LocalUser);
    user.color_rgb = 0x00FF00u;
    commit_slot_edit(kLane, user);

    Observation cache(ObservationSource::VendorCache);
    cache.spoolman_id = 7;
    cache.material = "PETG";
    ingest(kLane, cache);

    Observation sensed(ObservationSource::Sensed);
    sensed.present = true;
    ingest(kLane, sensed);

    Observation metered(ObservationSource::Metered);
    metered.remaining_weight_g = 612.0F;
    ingest(kLane, metered);

    REQUIRE(reconcile_binding(kLane, firmware_says(7)) == BindingVerdict::Rebound);

    const auto after = lane_sources(kLane);
    CHECK_FALSE(after.spoolman.has_value());
    CHECK_FALSE(after.local_user.has_value());
    // Firmware's own reading is what should paint once the stale ones are
    // gone, and presence and consumption were never identity.
    REQUIRE(after.vendor_cache.has_value());
    CHECK(after.vendor_cache->spoolman_id == 7);
    REQUIRE(after.sensed.has_value());
    CHECK(after.sensed->present == true);
    REQUIRE(after.metered.has_value());
    CHECK(after.metered->remaining_weight_g == Catch::Approx(612.0F));

    // What the lane now shows is firmware's statement, with no trace of the
    // record that named a spool nobody swapped in.
    const auto resolved = resolve(after);
    CHECK(resolved.spoolman_id == 7);
    // Unobserved, not blank. The dropped records were the only ones that ever
    // spoke to brand and colour, so nothing states them now, and a backend
    // that reports its own keeps them.
    CHECK_FALSE(resolved.brand.has_value());
    CHECK_FALSE(resolved.color_rgb.has_value());
}

TEST_CASE_METHOD(HelixTestFixture, "a user's hand-typed colour goes with the binding it described",
                 "[lane][binding]") {
    // The judgment this pins: a colour says what is loaded RIGHT NOW, and
    // firmware has just said what is loaded is a different spool. Keeping the
    // colour would paint the old spool's shade onto the new one.
    constexpr helix::ams::LaneId kLane = helix::ams::lane_id_for(0, 4);

    user_links(kLane, 42);
    Observation tint(ObservationSource::LocalUser);
    tint.color_rgb = 0x00FF00u;
    tint.color_name = "Lime";
    commit_slot_edit(kLane, tint);

    REQUIRE(lane_sources(kLane).local_user->color_rgb == 0x00FF00u);

    REQUIRE(reconcile_binding(kLane, firmware_says(169)) == BindingVerdict::Rebound);
    CHECK_FALSE(lane_sources(kLane).local_user.has_value());
}

TEST_CASE_METHOD(HelixTestFixture, "a colour on a lane bound to nothing survives a firmware id",
                 "[lane][binding]") {
    // The other half of the same judgment: with no declared binding there is
    // nothing for firmware to contradict, so the person's pick stands.
    constexpr helix::ams::LaneId kLane = helix::ams::lane_id_for(0, 5);

    Observation tint(ObservationSource::LocalUser);
    tint.color_rgb = 0x00FF00u;
    commit_slot_edit(kLane, tint);

    REQUIRE(reconcile_binding(kLane, firmware_says(169)) == BindingVerdict::Holds);
    REQUIRE(lane_sources(kLane).local_user.has_value());
    CHECK(lane_sources(kLane).local_user->color_rgb == 0x00FF00u);
}

TEST_CASE_METHOD(HelixTestFixture, "an ejected lane drops the same two records a re-bind does",
                 "[lane][binding]") {
    constexpr helix::ams::LaneId kLane = helix::ams::lane_id_for(0, 6);

    Observation server(ObservationSource::Spoolman);
    server.spoolman_id = 42;
    ingest(kLane, server);
    user_links(kLane, 42);

    REQUIRE(reconcile_binding(kLane, id_reporting_firmware_says(0, false)) ==
            BindingVerdict::Ejected);
    CHECK_FALSE(lane_sources(kLane).spoolman.has_value());
    CHECK_FALSE(lane_sources(kLane).local_user.has_value());
}

TEST_CASE_METHOD(HelixTestFixture, "a binding that holds costs the lane nothing",
                 "[lane][binding]") {
    constexpr helix::ams::LaneId kLane = helix::ams::lane_id_for(0, 7);

    Observation server(ObservationSource::Spoolman);
    server.spoolman_id = 42;
    server.brand = "Polymaker";
    ingest(kLane, server);
    user_links(kLane, 42);

    REQUIRE(reconcile_binding(kLane, firmware_says(42)) == BindingVerdict::Holds);
    REQUIRE(lane_sources(kLane).spoolman.has_value());
    CHECK(lane_sources(kLane).spoolman->brand == "Polymaker");
    CHECK(lane_sources(kLane).local_user.has_value());
}

TEST_CASE_METHOD(HelixTestFixture, "dropping a source leaves every other source standing",
                 "[lane][binding]") {
    constexpr helix::ams::LaneId kLane = helix::ams::lane_id_for(0, 8);

    Observation cache(ObservationSource::VendorCache);
    cache.material = "PETG";
    ingest(kLane, cache);
    Observation sensed(ObservationSource::Sensed);
    sensed.present = true;
    ingest(kLane, sensed);

    drop_lane_source(kLane, ObservationSource::VendorCache);

    const auto after = lane_sources(kLane);
    CHECK_FALSE(after.vendor_cache.has_value());
    REQUIRE(after.sensed.has_value());
    CHECK(after.sensed->present == true);
}

TEST_CASE_METHOD(HelixTestFixture, "a drop that names no lane writes no lane", "[lane][binding]") {
    // The funnels refuse an id the scheme does not assign; a drop must not
    // create the entry they refused to write.
    drop_lane_source(helix::ams::kInvalidLaneId, ObservationSource::Spoolman);
    drop_lane_source(helix::ams::lane_id_for(0, 9), ObservationSource::Spoolman);

    CHECK(helix::ams::known_lanes().empty());
}

// ============================================================================
// The call sites - a backend must reach this on its own status path
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "an AFC lane stops painting a spool that was swapped away",
                 "[lane][binding][afc]") {
    // The defect in one case: a person links a spool, someone swaps it on the
    // machine, and the lane paints the old one forever because LocalUser
    // outranks firmware's own cache in the identity ladder.
    SettingsManager::instance().init_subjects();

    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane1", {{"prep", true}, {"status", "Loaded"}, {"spool_id", 42}});
    user_links(harness.lane(0), 42);
    REQUIRE(resolve(lane_sources(harness.lane(0))).spoolman_id == 42);

    // Mainsail or the AFC plugin binds a different spool.
    feed_afc_lane(*harness, "lane1", {{"spool_id", 169}});

    const auto after = lane_sources(harness.lane(0));
    CHECK_FALSE(after.local_user.has_value());
    CHECK(resolve(after).spoolman_id == 169);
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's own re-link is not read back as someone else's",
                 "[lane][binding][afc]") {
    SettingsManager::instance().init_subjects();

    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane1", {{"prep", true}, {"status", "Loaded"}, {"spool_id", 42}});
    user_links(harness.lane(0), 169);
    {
        std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(*harness));
        AfcTestAccess::record_own_spool_write(*harness, 0, 169, 42);
    }

    // An in-flight frame still reporting the old id is our own write, so the
    // declaration we just filed has to survive it.
    feed_afc_lane(*harness, "lane1", {{"spool_id", 42}});
    REQUIRE(lane_sources(harness.lane(0)).local_user.has_value());
    CHECK(lane_sources(harness.lane(0)).local_user->spoolman_id == 169);

    // The echo lands and still changes nothing.
    feed_afc_lane(*harness, "lane1", {{"spool_id", 169}});
    CHECK(lane_sources(harness.lane(0)).local_user.has_value());

    // A third id is a genuine external change and does take the record.
    feed_afc_lane(*harness, "lane1", {{"spool_id", 200}});
    CHECK_FALSE(lane_sources(harness.lane(0)).local_user.has_value());
    CHECK(resolve(lane_sources(harness.lane(0))).spoolman_id == 200);
}

TEST_CASE_METHOD(LVGLTestFixture, "a relink made through the edit path survives the stale frame",
                 "[lane][binding][afc]") {
    // The same race the case above covers, driven the way the UI drives it.
    // Nothing here stages an expectation by hand: recording one is the edit
    // path's own job, so a save that stopped recording is what this case
    // exists to catch. Without it the stale frame reads as an external
    // re-bind, and reconcile_binding drops the declaration the save just
    // filed along with the stored record behind it.
    SettingsManager::instance().init_subjects();

    helix::test::RegisteredBackend<GcodeCapturingAfc> harness;
    init_afc_lanes(*harness);
    feed_afc_lane(*harness, "lane1", {{"prep", true}, {"status", "Loaded"}});

    auto& ams = helix::AmsState::instance();

    // The user links spool 42. LocalUser is the only source naming an id:
    // there is no Spoolman record on this lane.
    const SlotInfo before_link = harness->get_slot_info(0);
    SlotInfo linked = before_link;
    linked.spoolman_id = 42;
    REQUIRE(ams.commit_slot_edit(0, before_link, linked).success());
    REQUIRE(harness->sent_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=42"));
    REQUIRE(lane_sources(harness.lane(0)).local_user.has_value());
    REQUIRE(lane_sources(harness.lane(0)).local_user->spoolman_id == 42);
    REQUIRE_FALSE(lane_sources(harness.lane(0)).spoolman.has_value());

    // Firmware echoes the write, so the two agree before the re-link.
    feed_afc_lane(*harness, "lane1", {{"spool_id", 42}});
    REQUIRE(resolve(lane_sources(harness.lane(0))).spoolman_id == 42);

    // The user re-links to 99 through the same path. The save amends
    // LocalUser, records that 42 is about to become 99, and dispatches.
    const SlotInfo before_relink = harness->get_slot_info(0);
    REQUIRE(before_relink.spoolman_id == 42);
    SlotInfo relinked = before_relink;
    relinked.spoolman_id = 99;
    REQUIRE(ams.commit_slot_edit(0, before_relink, relinked).success());

    // The dispatch is the proof the save ran its write path. Every assertion
    // below is that something SURVIVES, which an edit that returned early
    // would satisfy just as well.
    REQUIRE(harness->sent_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=99"));
    REQUIRE(lane_sources(harness.lane(0)).local_user->spoolman_id == 99);

    // AFC keeps reporting the superseded id for a poll or two. The verdict has
    // to be Holds, and these are what a Holds leaves standing: a Rebound drops
    // the declaring records and clears the stored one behind them.
    feed_afc_lane(*harness, "lane1", {{"spool_id", 42}});

    const auto after = lane_sources(harness.lane(0));
    CHECK(after.local_user.has_value());
    CHECK(after.local_user->spoolman_id == 99);
    CHECK(resolve(after).spoolman_id == 99);
    CHECK(harness->get_slot_info(0).spoolman_id == 99);
    {
        std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(*harness));
        const auto& stored = AfcTestAccess::overrides(*harness);
        REQUIRE(stored.count(0) == 1);
        CHECK(stored.at(0).spoolman_id == 99);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "a frame silent about the spool id cannot end an own write",
                 "[lane][binding][afc]") {
    // AFC sends deltas, so most frames between our write and its echo say
    // nothing about spool_id. Only the reader holding firmware's own id may
    // end the expectation: a reader passing the merged SlotInfo sees the id we
    // just wrote, and consuming on that would leave this frame reading our own
    // in-flight write as somebody else's re-bind.
    SettingsManager::instance().init_subjects();

    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    user_links(harness.lane(0), 169);
    {
        std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(*harness));
        helix::ams::FilamentSlotOverride stored;
        stored.spoolman_id = 169;
        stored.brand = "Polymaker";
        AfcTestAccess::overrides(*harness)[0] = stored;
        AfcTestAccess::record_own_spool_write(*harness, 0, 169, 42);
    }

    // A stale pre-echo frame. The merge writes the stored 169 into the
    // SlotInfo, so from here the merged struct and firmware disagree.
    feed_afc_lane(*harness, "lane1", {{"prep", true}, {"status", "Loaded"}, {"spool_id", 42}});
    REQUIRE(lane_sources(harness.lane(0)).local_user.has_value());

    // The commonest AFC frame: a status delta naming no spool.
    feed_afc_lane(*harness, "lane1", {{"status", "Tooled"}});

    REQUIRE(lane_sources(harness.lane(0)).local_user.has_value());
    CHECK(lane_sources(harness.lane(0)).local_user->spoolman_id == 169);
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC honours the retention setting when a lane is ejected",
                 "[lane][binding][afc]") {
    auto& settings = SettingsManager::instance();
    settings.init_subjects();
    settings.set_ams_keep_spool_info_on_eject(true);

    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane1", {{"prep", true}, {"status", "Loaded"}, {"spool_id", 42}});
    user_links(harness.lane(0), 42);

    // AFC writes spool_id=None on eject. With retention on the lane ghosts:
    // presence goes false while the identity keeps standing.
    feed_afc_lane(*harness, "lane1", {{"prep", false}, {"load", false}, {"spool_id", nullptr}});
    REQUIRE(lane_sources(harness.lane(0)).local_user.has_value());
    CHECK(resolve(lane_sources(harness.lane(0))).spoolman_id == 42);

    settings.set_ams_keep_spool_info_on_eject(false);
    feed_afc_lane(*harness, "lane2", {{"prep", true}, {"status", "Loaded"}, {"spool_id", 7}});
    user_links(harness.lane(1), 7);
    REQUIRE(lane_sources(harness.lane(1)).local_user.has_value());

    feed_afc_lane(*harness, "lane2", {{"prep", false}, {"load", false}, {"spool_id", nullptr}});
    CHECK_FALSE(lane_sources(harness.lane(1)).local_user.has_value());
    // Nobody declares a spool on this lane any more. Unobserved rather than
    // zero: AFC's own record reset the id when firmware wrote None, so there
    // is no reading at all, which is a stronger statement than a zero.
    CHECK_FALSE(resolve(lane_sources(harness.lane(1))).spoolman_id.has_value());

    settings.set_ams_keep_spool_info_on_eject(true);
}

TEST_CASE_METHOD(LVGLTestFixture, "a Happy Hare gate re-bound in the gate map drops its record",
                 "[lane][binding][happy_hare]") {
    SettingsManager::instance().init_subjects();

    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_spool_id", nlohmann::json::array({42, 0})}});
    user_links(harness.lane(0), 42);
    REQUIRE(resolve(lane_sources(harness.lane(0))).spoolman_id == 42);

    feed_mmu(*harness, {{"gate_spool_id", nlohmann::json::array({169, 0})}});

    CHECK_FALSE(lane_sources(harness.lane(0)).local_user.has_value());
    CHECK(resolve(lane_sources(harness.lane(0))).spoolman_id == 169);

    // Gate 1 declared nothing and states nothing, so it is untouched.
    CHECK_FALSE(lane_sources(harness.lane(1)).local_user.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a flat-schema CFS bay re-bound by the fork drops its record",
                 "[lane][binding][cfs]") {
    SettingsManager::instance().init_subjects();

    CfsHarness harness;

    feed_cfs_flat(*harness, 42);
    user_links(harness.lane(0), 42);
    REQUIRE(resolve(lane_sources(harness.lane(0))).spoolman_id == 42);

    feed_cfs_flat(*harness, 169);

    CHECK_FALSE(lane_sources(harness.lane(0)).local_user.has_value());
    CHECK(resolve(lane_sources(harness.lane(0))).spoolman_id == 169);
}

TEST_CASE_METHOD(LVGLTestFixture, "a CFS bay reading zero is never an eject",
                 "[lane][binding][cfs]") {
    // printer_reports_spool_ids() is false on CFS, so 0 is the everyday
    // reading. A retention-off user must not have every bay emptied.
    auto& settings = SettingsManager::instance();
    settings.init_subjects();
    settings.set_ams_keep_spool_info_on_eject(false);

    CfsHarness harness;

    feed_cfs_flat(*harness, 42);
    user_links(harness.lane(0), 42);
    feed_cfs_flat(*harness, 0);

    REQUIRE(lane_sources(harness.lane(0)).local_user.has_value());
    CHECK(lane_sources(harness.lane(0)).local_user->spoolman_id == 42);

    settings.set_ams_keep_spool_info_on_eject(true);
}

// ============================================================================
// classify_insert (prestonbrown/helixscreen#1710)
// ============================================================================

namespace {

using helix::ams::classify_insert;
using helix::ams::InsertVerdict;
using helix::ams::SpoolEvidence;

SpoolEvidence tag(std::string uid, std::string material = "", std::optional<uint32_t> rgb = {}) {
    return SpoolEvidence{std::move(uid), std::move(material), rgb};
}

SpoolEvidence read(std::string material, std::optional<uint32_t> rgb) {
    return SpoolEvidence{"", std::move(material), rgb};
}

} // namespace

TEST_CASE("an insert with no reading of the spool before it has no evidence",
          "[lane][insert_rule]") {
    CHECK(classify_insert(std::nullopt, tag("04A1")) == InsertVerdict::NoEvidence);
    CHECK(classify_insert(std::nullopt, read("PLA", 0xFF0000)) == InsertVerdict::NoEvidence);
}

TEST_CASE("an untagged insert that reads nothing has no evidence", "[lane][insert_rule]") {
    CHECK(classify_insert(read("PLA", 0xFF0000), SpoolEvidence{}) == InsertVerdict::NoEvidence);
    CHECK(classify_insert(SpoolEvidence{}, SpoolEvidence{}) == InsertVerdict::NoEvidence);
}

TEST_CASE("a tag UID on both sides decides alone", "[lane][insert_rule]") {
    SECTION("the same tag is the same spool even with its contents rewritten") {
        CHECK(classify_insert(tag("04A1", "PLA", 0xFF0000), tag("04A1", "PETG", 0x0000FF)) ==
              InsertVerdict::SameSpool);
    }
    SECTION("a new tag is a new spool even with identical contents") {
        CHECK(classify_insert(tag("04A1", "PLA", 0xFF0000), tag("04B2", "PLA", 0xFF0000)) ==
              InsertVerdict::DifferentSpool);
    }
}

TEST_CASE("a tag on one side only falls through to material and colour", "[lane][insert_rule]") {
    CHECK(classify_insert(tag("04A1", "PLA", 0xFF0000), read("PLA", 0xFF0000)) ==
          InsertVerdict::SameSpool);
    CHECK(classify_insert(read("PLA", 0xFF0000), tag("04A1", "PETG", 0xFF0000)) ==
          InsertVerdict::DifferentSpool);
    CHECK(classify_insert(tag("04A1"), read("PLA", 0xFF0000)) == InsertVerdict::NoEvidence);
}

TEST_CASE("identical material and colour are the same spool", "[lane][insert_rule]") {
    CHECK(classify_insert(read("PLA", 0xFF0000), read("PLA", 0xFF0000)) ==
          InsertVerdict::SameSpool);
    SECTION("material compares without case") {
        CHECK(classify_insert(read("petg", 0x00FF00), read("PETG", 0x00FF00)) ==
              InsertVerdict::SameSpool);
    }
    SECTION("colour compares on RGB, ignoring any alpha byte") {
        CHECK(classify_insert(read("PLA", 0xFF00FF00u), read("PLA", 0x0000FF00u)) ==
              InsertVerdict::SameSpool);
    }
}

TEST_CASE("any field read on both sides that differs is a different spool", "[lane][insert_rule]") {
    CHECK(classify_insert(read("PLA", 0xFF0000), read("PETG", 0xFF0000)) ==
          InsertVerdict::DifferentSpool);
    CHECK(classify_insert(read("PLA", 0xFF0000), read("PLA", 0xFF0001)) ==
          InsertVerdict::DifferentSpool);
    SECTION("one differing field is enough when the other went unread") {
        CHECK(classify_insert(read("PLA", std::nullopt), read("PETG", 0xFF0000)) ==
              InsertVerdict::DifferentSpool);
        CHECK(classify_insert(read("", 0xFF0000), read("PLA", 0x00FF00)) ==
              InsertVerdict::DifferentSpool);
    }
    SECTION("a similar material family is still a different spool") {
        CHECK(classify_insert(read("PLA", 0xFF0000), read("PLA-CF", 0xFF0000)) ==
              InsertVerdict::DifferentSpool);
    }
}

namespace {

SpoolEvidence untagged_read(std::string material, std::optional<uint32_t> rgb) {
    SpoolEvidence e{"", std::move(material), rgb};
    e.tag_read_complete = true;
    return e;
}

} // namespace

TEST_CASE("a finished tag read decides when a tag appears or disappears", "[lane][insert_rule]") {
    SECTION("a tagged spool replacing an untagged one is a new spool") {
        CHECK(classify_insert(untagged_read("PLA", 0xFF0000), tag("04A1", "PLA", 0xFF0000)) ==
              InsertVerdict::DifferentSpool);
    }
    SECTION("an untagged spool replacing a tagged one is a new spool") {
        CHECK(classify_insert(tag("04A1", "PLA", 0xFF0000), untagged_read("PLA", 0xFF0000)) ==
              InsertVerdict::DifferentSpool);
    }
    SECTION("a read not yet finished falls through to material and colour") {
        CHECK(classify_insert(tag("04A1", "PLA", 0xFF0000), read("PLA", 0xFF0000)) ==
              InsertVerdict::SameSpool);
        CHECK(classify_insert(read("PLA", 0xFF0000), tag("04A1", "PLA", 0xFF0000)) ==
              InsertVerdict::SameSpool);
    }
    SECTION("two finished reads with no tag fall through to material and colour") {
        CHECK(classify_insert(untagged_read("PLA", 0xFF0000), untagged_read("PLA", 0xFF0000)) ==
              InsertVerdict::SameSpool);
        CHECK(classify_insert(untagged_read("PLA", 0xFF0000), untagged_read("ABS", 0xFF0000)) ==
              InsertVerdict::DifferentSpool);
    }
}

TEST_CASE("material ignores surrounding whitespace", "[lane][insert_rule]") {
    CHECK(classify_insert(read(" PLA ", 0xFF0000), read("pla", 0xFF0000)) ==
          InsertVerdict::SameSpool);
    CHECK(classify_insert(read("  ", 0xFF0000), read("", 0xFF0000)) == InsertVerdict::NoEvidence);
}

TEST_CASE("one matching field alone is not the same spool", "[lane][insert_rule]") {
    CHECK(classify_insert(read("PLA", std::nullopt), read("PLA", 0xFF0000)) ==
          InsertVerdict::NoEvidence);
    CHECK(classify_insert(read("", 0xFF0000), read("", 0xFF0000)) == InsertVerdict::NoEvidence);
}
