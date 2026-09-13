// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What each backend's own signal becomes in the lane source model. Cases here
// assert on the POPULATED SOURCES; a SlotInfo read appears only where one is
// needed to establish a case's precondition.

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "ams_backend_ace.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_backend_qidi.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "filament_slot_override.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/ace_test_access.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/happy_hare_test_access.h"
#include "test_helpers/qidi_box_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_runtime_config.h"
#include "test_helpers/snapmaker_test_access.h"
#include "test_helpers/toolchanger_test_access.h"
#include "toolchanger_addon.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AceTestAccess;
using helix::Ad5xIfsTestAccess;
using helix::AfcTestAccess;
using helix::AmsBackendAce;
using helix::AmsBackendAd5xIfs;
using helix::AmsBackendAfc;
using helix::AmsBackendHappyHare;
using helix::AmsBackendMock;
using helix::AmsBackendQidi;
using helix::AmsBackendSnapmaker;
using helix::AmsBackendToolChanger;
using helix::CfsTestAccess;
using helix::HappyHareTestAccess;
using helix::QidiBoxTestAccess;
using helix::SnapmakerTestAccess;
using helix::ToolChangerTestAccess;
using helix::ams::lane_sources;
using helix::printer::AmsBackendCfs;
using helix::test::RegisteredBackend;

namespace {
/// A backend with no Moonraker behind it, registered so its lane ids are real.
using Ad5xHarness = RegisteredBackend<AmsBackendAd5xIfs>;
using AfcHarness = RegisteredBackend<AmsBackendAfc>;
using HappyHareHarness = RegisteredBackend<AmsBackendHappyHare>;
using CfsHarness = RegisteredBackend<AmsBackendCfs>;
using AceHarness = RegisteredBackend<AmsBackendAce>;
using SnapmakerHarness = RegisteredBackend<AmsBackendSnapmaker>;
using ToolChangerHarness = RegisteredBackend<AmsBackendToolChanger>;
using QidiHarness = RegisteredBackend<AmsBackendQidi>;
using MockHarness = RegisteredBackend<AmsBackendMock>;

/// One `box` object, delivered the way Moonraker delivers it. Which schema
/// parsed, and whether the frame counts as a full update at all, are decisions
/// the production entry point makes, so every CFS case drives that rather than
/// one of the two static parsers.
void feed_cfs_box(AmsBackendCfs& backend, const nlohmann::json& box) {
    nlohmann::json params;
    params["box"] = box;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    CfsTestAccess::handle_status(backend, notification);
}

/// A community-fork `box` payload: a flat self-describing slots[] array.
nlohmann::json flat_box(nlohmann::json slots) {
    return nlohmann::json{{"api_version", 1}, {"slots", std::move(slots)}};
}

/// A stock Creality `box` payload carrying one unit's four bays. `filament` is
/// what marks the frame a full update on this schema.
nlohmann::json stock_box(const std::string& unit, nlohmann::json unit_json) {
    nlohmann::json box{{"filament", 0}};
    unit_json["state"] = "connect";
    box[unit] = std::move(unit_json);
    return box;
}

/// One hub status object, through the notify envelope the subscription path
/// unwraps. `slots` is a single Klipper status field, so Moonraker sends the
/// array whole: unlike AFC and Happy Hare there is no sub-field delta to model.
void feed_ace(AmsBackendAce& backend, const nlohmann::json& data) {
    nlohmann::json params;
    params["ace"] = data;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    AceTestAccess::handle_status_update(backend, notification);
}

/// A user colour and material that disagree with whatever firmware reports.
helix::ams::FilamentSlotOverride user_colour_and_material() {
    helix::ams::FilamentSlotOverride user;
    user.color_rgb = 0x00FF00u;
    user.color_set = true;
    user.user_locked_color = true;
    user.material = "ABS";
    return user;
}

/// One `[fila<N>]` section, the shape apply_filas_list() reads out of the
/// Box's officiall_filas_list.cfg.
std::string fila_section(int id, const std::string& name, const std::string& type) {
    return "[fila" + std::to_string(id) + "]\nfilament = " + name + "\ntype = " + type +
           "\nmin_temp = 230\nmax_temp = 250\n";
}

/// A filas list carrying one profile, one colour and one vendor row.
std::string filas_list(int fila_id, const std::string& type, int color_id,
                       const std::string& color_hex, int vendor_id, const std::string& vendor) {
    return fila_section(fila_id, type + " Basic", type) + "[colordict]\n" +
           std::to_string(color_id) + " = " + color_hex + "\n[vendor_list]\n" +
           std::to_string(vendor_id) + " = " + vendor + "\n";
}

/// Four lanes through AFC's own initialize_slots(), which is what a discovery
/// answer ends in.
void init_afc_lanes(AmsBackendAfc& backend) {
    AfcTestAccess::initialize_slots(backend,
                                    std::vector<std::string>{"lane1", "lane2", "lane3", "lane4"});
}

/// One AFC lane_data payload, the Moonraker DB snapshot, keyed by lane name.
void feed_afc_lane_data(AmsBackendAfc& backend, const nlohmann::json& lane_data) {
    std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(backend));
    AfcTestAccess::parse_lane_data(backend, lane_data);
}

/// One AFC_stepper lane object, delivered the way Moonraker delivers it: a
/// notify_status_update carrying only the keys that changed. Every case here
/// drives the production path rather than the parse alone, because what makes
/// AFC different is that its frames are deltas.
void feed_afc_lane(AmsBackendAfc& backend, const std::string& lane_name,
                   const nlohmann::json& data) {
    nlohmann::json params;
    params["AFC_stepper " + lane_name] = data;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    AfcTestAccess::handle_status_update(backend, notification);
}

/// One printer.mmu object, delivered the way Moonraker delivers it: a
/// notify_status_update carrying only the keys that changed. Happy Hare's
/// frames are deltas, so every case here drives the production entry point
/// rather than the parse alone.
void feed_mmu(AmsBackendHappyHare& backend, const nlohmann::json& mmu) {
    nlohmann::json params;
    params["mmu"] = mmu;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    HappyHareTestAccess::handle_status_update(backend, notification);
}

/// One filament_detect object, through the notify envelope handle_status_update
/// unwraps. RFID identity and channel presence arrive under the same key, which
/// is why one frame feeds both of this backend's records.
void feed_filament_detect(AmsBackendSnapmaker& backend, const nlohmann::json& fd) {
    nlohmann::json params;
    params["filament_detect"] = fd;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    SnapmakerTestAccess::handle_status(backend, notification);
}

/// One print_task_config object, through the same envelope. This is Snapmaker's
/// second identity writer and the one that files no observation, so a case
/// needs to drive it separately from filament_detect.
void feed_print_task_config(AmsBackendSnapmaker& backend, const nlohmann::json& ptc) {
    nlohmann::json params;
    params["print_task_config"] = ptc;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    SnapmakerTestAccess::handle_status(backend, notification);
}

/// One tool-changer status object, delivered the way Moonraker delivers it.
void feed_toolchanger(AmsBackendToolChanger& backend, const nlohmann::json& status) {
    nlohmann::json notification;
    notification["method"] = "notify_status_update";
    notification["params"] = nlohmann::json::array({status, 0.0});
    ToolChangerTestAccess::handle_status(backend, notification);
}

/// A stock upstream MedusaHC object list, which is what gives a tool changer
/// dock sensors at all. Without them every toolhead is assumed present and
/// EMPTY is not an answer the backend can reach.
helix::PrinterDiscovery medusahc_discovery() {
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "tool T2",
                                            "pin_watch io", "servo my_servo", "extruder"}));
    return hw;
}
} // namespace

/// The presence vocabulary every producer in this file consumes, pinned over
/// the whole enum. A producer's own case can only reach the statuses its
/// firmware emits, so no backend test covers the rule end to end.
TEST_CASE("the presence vocabulary answers for every slot status", "[lane][ingest]") {
    using helix::slot_status_reports_filament;
    using helix::SlotStatus;

    auto stated = [](SlotStatus status) { return slot_status_reports_filament(status); };

    const auto empty = stated(SlotStatus::EMPTY);
    REQUIRE(empty.has_value());
    CHECK(*empty == false);

    for (SlotStatus occupied :
         {SlotStatus::AVAILABLE, SlotStatus::LOADED, SlotStatus::FROM_BUFFER}) {
        INFO("status " << helix::slot_status_to_string(occupied));
        const auto reading = stated(occupied);
        REQUIRE(reading.has_value());
        CHECK(*reading == true);
    }

    // A jam is filament stuck in the path, so a blocked bay holds filament.
    // QIDI reports it for any negative state word, and both SlotInfo::
    // is_present() and helix::ui::classify_lane() count it present; answering
    // "no reading" here would retract a live reading the moment a lane jams.
    const auto blocked = stated(SlotStatus::BLOCKED);
    REQUIRE(blocked.has_value());
    CHECK(*blocked == true);

    // The one status where this rule and is_present() differ, and the reason
    // the answer is three-valued at all.
    CHECK_FALSE(stated(SlotStatus::UNKNOWN).has_value());

    // The contract between the two rules, asserted rather than described: they
    // agree on every status but UNKNOWN. Either one drifting fails here.
    for (SlotStatus status : {SlotStatus::EMPTY, SlotStatus::AVAILABLE, SlotStatus::LOADED,
                              SlotStatus::FROM_BUFFER, SlotStatus::BLOCKED}) {
        INFO("status " << helix::slot_status_to_string(status));
        helix::SlotInfo slot;
        slot.status = status;
        const auto reading = stated(status);
        REQUIRE(reading.has_value());
        CHECK(*reading == slot.is_present());
    }

    helix::SlotInfo unknown_slot;
    unknown_slot.status = SlotStatus::UNKNOWN;
    CHECK_FALSE(unknown_slot.is_present());
}

TEST_CASE_METHOD(LVGLTestFixture, "a registered backend's slots are its own block of lanes",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);

    CHECK(harness.lane(0) == helix::ams::lane_id_for(0, 0));
    CHECK(harness.lane(3) == helix::ams::lane_id_for(0, 3));
    CHECK(helix::ams::is_lane_id(harness.lane(0)));
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X ingests silk-sensor presence as sensed",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_color(*harness, 0, "ED2C2C");
    Ad5xIfsTestAccess::set_material(*harness, 0, "PETG");
    Ad5xIfsTestAccess::set_port_presence(*harness, 0, false);

    const auto lane = lane_sources(harness.lane(0));

    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == false);
    // The vendor file still remembers the last spool. That is a cache of a past
    // declaration, so it must not carry presence or identity into the sensor's
    // record.
    CHECK_FALSE(lane.sensed->color_rgb.has_value());
    CHECK_FALSE(lane.sensed->material.has_value());

    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    REQUIRE(lane.vendor_cache->material.has_value());
    CHECK(*lane.vendor_cache->material == "PETG");
    CHECK_FALSE(lane.vendor_cache->present.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X files a sensed record with no reading before Ports is seen",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);

    // Native ZMOD publishes no per-port sensors, so port_presence_ is false for
    // every lane whether or not filament is there. An unread sensor is not a
    // reading of "empty".
    Ad5xIfsTestAccess::set_port_presence(*harness, 1, false);

    const auto unread = lane_sources(harness.lane(1));
    // The record must exist: its absence would mean the translation never ran,
    // which is a different fact from "the sensor has said nothing yet" and must
    // not pass for it.
    REQUIRE(unread.sensed.has_value());
    CHECK_FALSE(unread.sensed->present.has_value());

    // The same lane, once the silk mask has been read. This half is what makes
    // the half above a guard rather than a backend that never reports presence.
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_port_presence(*harness, 1, false);

    const auto read = lane_sources(harness.lane(1));
    REQUIRE(read.sensed.has_value());
    REQUIRE(read.sensed->present.has_value());
    CHECK(*read.sensed->present == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X does not cache a colour it has not read",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_color(*harness, 2, "");
    Ad5xIfsTestAccess::set_material(*harness, 2, "");
    Ad5xIfsTestAccess::set_port_presence(*harness, 2, true);

    const auto lane = lane_sources(harness.lane(2));

    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);

    REQUIRE(lane.vendor_cache.has_value());
    CHECK_FALSE(lane.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(lane.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X caches pure black as a colour", "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_port_presence(*harness, 3, true);
    Ad5xIfsTestAccess::set_color(*harness, 3, "000000");

    const auto lane = lane_sources(harness.lane(3));

    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0x000000u);
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X files no colour when the stored one will not parse",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_port_presence(*harness, 0, true);
    Ad5xIfsTestAccess::set_color(*harness, 0, "ED2C2C");

    const auto parsed = lane_sources(harness.lane(0));
    REQUIRE(parsed.vendor_cache.has_value());
    REQUIRE(parsed.vendor_cache->color_rgb == 0xED2C2Cu);

    // parse_adventurer_json stores ffmColorN as the printer sent it, so
    // colors_[] can hold a string that is not hex. The colour already on the
    // lane must not stand in for one: the reading is gone, and "no colour
    // observed" is a state this model can express.
    Ad5xIfsTestAccess::set_color(*harness, 0, "notahexvalue");

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK_FALSE(lane.vendor_cache->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches AD5X's vendor-cache record",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);

    // A user colour that disagrees with what the vendor file says. SlotInfo is
    // persistent across frames and apply_overrides rewrites it in place, so
    // entry->info.color_rgb is this value by the time the frame ends. A
    // translation reading it back would file the user's own choice as something
    // the vendor store remembers.
    helix::ams::FilamentSlotOverride user;
    user.color_rgb = 0x00FF00u;
    user.color_set = true;
    user.user_locked_color = true;
    Ad5xIfsTestAccess::seed_override(*harness, 1, user);

    Ad5xIfsTestAccess::set_color(*harness, 1, "ED2C2C");
    Ad5xIfsTestAccess::set_port_presence(*harness, 1, true);

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot, there is no laundering for the case to catch and
    // the assertion below would hold for the wrong reason.
    REQUIRE(harness->get_slot_info(1).color_rgb == 0x00FF00u);

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);

    // A lane the vendor file says nothing about. Here entry->info.color_rgb
    // holds the override alone, so a read-back files a colour where the vendor
    // store has none.
    Ad5xIfsTestAccess::seed_override(*harness, 2, user);
    Ad5xIfsTestAccess::set_port_presence(*harness, 2, true);
    Ad5xIfsTestAccess::set_port_presence(*harness, 2, true);

    REQUIRE(harness->get_slot_info(2).color_rgb == 0x00FF00u);

    const auto blank = lane_sources(harness.lane(2));
    REQUIRE(blank.vendor_cache.has_value());
    CHECK_FALSE(blank.vendor_cache->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC splits its sensors, its own store and its own weight",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane1",
                  {{"prep", true},
                   {"load", true},
                   {"tool_loaded", false},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"},
                   {"spool_vendor", "Kingroon"},
                   {"weight", 612.0},
                   {"spool_id", 7},
                   {"initial_weight", 1000.0}});

    const auto lane = lane_sources(harness.lane(0));

    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);
    // prep and load are real sensors and say nothing about which spool this is.
    CHECK_FALSE(lane.sensed->color_rgb.has_value());
    CHECK_FALSE(lane.sensed->material.has_value());

    // AFC's own store keeps colour, material, filament name and vendor across
    // an eject, so all four are a cache of a past declaration.
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->material == "PETG");
    CHECK(lane.vendor_cache->spool_name == "Galaxy Black");
    CHECK(lane.vendor_cache->brand == "Kingroon");
    CHECK(lane.vendor_cache->spoolman_id == 7);
    CHECK_FALSE(lane.vendor_cache->present.has_value());

    REQUIRE(lane.metered.has_value());
    REQUIRE(lane.metered->remaining_weight_g.has_value());
    CHECK(*lane.metered->remaining_weight_g == Catch::Approx(612.0F));
    REQUIRE(lane.metered->total_weight_g.has_value());
    CHECK(*lane.metered->total_weight_g == Catch::Approx(1000.0F));
    // A weight tick has no business asserting identity or presence.
    CHECK_FALSE(lane.metered->material.has_value());
    CHECK_FALSE(lane.metered->present.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a partial AFC delta narrows nothing it does not mention",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane2",
                  {{"prep", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"}});

    // The commonest frame AFC sends: the lane reached the toolhead and nothing
    // about the spool changed. A record assembled from this frame alone would
    // leave the lane with no identity at all.
    feed_afc_lane(*harness, "lane2", {{"status", "Tooled"}});

    const auto after_status = lane_sources(harness.lane(1));
    REQUIRE(after_status.vendor_cache.has_value());
    REQUIRE(after_status.vendor_cache->color_rgb.has_value());
    CHECK(*after_status.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(after_status.vendor_cache->material == "PETG");
    CHECK(after_status.vendor_cache->spool_name == "Galaxy Black");
    REQUIRE(after_status.sensed.has_value());
    CHECK(after_status.sensed->present == true);

    // A colour change alone must not take the material with it.
    feed_afc_lane(*harness, "lane2", {{"color", "#00AEFF"}});

    const auto after_color = lane_sources(harness.lane(1));
    REQUIRE(after_color.vendor_cache.has_value());
    REQUIRE(after_color.vendor_cache->color_rgb.has_value());
    CHECK(*after_color.vendor_cache->color_rgb == 0x00AEFFu);
    CHECK(after_color.vendor_cache->material == "PETG");
    CHECK(after_color.vendor_cache->spool_name == "Galaxy Black");
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC files no weight reading until a frame carries one",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane4", {{"prep", true}, {"status", "Loaded"}});

    const auto lane = lane_sources(harness.lane(3));
    // The record is filed whether or not AFC has weighed anything, so the
    // absence below is a reading AFC has not made rather than a translation
    // that never ran.
    REQUIRE(lane.metered.has_value());
    CHECK_FALSE(lane.metered->remaining_weight_g.has_value());
    CHECK_FALSE(lane.metered->total_weight_g.has_value());

    feed_afc_lane(*harness, "lane4", {{"weight", 612.0}});

    const auto weighed = lane_sources(harness.lane(3));
    REQUIRE(weighed.metered.has_value());
    REQUIRE(weighed.metered->remaining_weight_g.has_value());
    CHECK(*weighed.metered->remaining_weight_g == Catch::Approx(612.0F));
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's eject clears its own store without clearing the sensor",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane3",
                  {{"prep", true},
                   {"load", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"}});
    REQUIRE(lane_sources(harness.lane(2)).vendor_cache->material == "PETG");

    // clear_values() empties colour, material and filament name on eject. An
    // empty value is a clear, so the cache must stop reporting them rather
    // than standing on the last frame that named them.
    feed_afc_lane(*harness, "lane3",
                  {{"prep", false},
                   {"load", false},
                   {"status", "None"},
                   {"color", ""},
                   {"material", ""},
                   {"filament_name", ""}});

    const auto lane = lane_sources(harness.lane(2));
    REQUIRE(lane.sensed.has_value());
    CHECK(lane.sensed->present == false);
    REQUIRE(lane.vendor_cache.has_value());
    CHECK_FALSE(lane.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(lane.vendor_cache->material.has_value());
    CHECK_FALSE(lane.vendor_cache->spool_name.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches AFC's vendor-cache record",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // A user colour and material that disagree with what AFC reports.
    // apply_overrides() rewrites SlotInfo with these on every frame, so a
    // translation reading that struct back would file the user's own choice as
    // something AFC's store remembers.
    helix::ams::FilamentSlotOverride user;
    user.color_rgb = 0x00FF00u;
    user.color_set = true;
    user.user_locked_color = true;
    user.material = "ABS";
    AfcTestAccess::overrides(*harness)[1] = user;

    feed_afc_lane(
        *harness, "lane2",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PETG"}});

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot there is no laundering for this case to catch
    // and both assertions below would hold for the wrong reason.
    const auto merged = harness->get_slot_info(1);
    REQUIRE(merged.color_rgb == 0x00FF00u);
    REQUIRE(merged.material == "ABS");

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a frame with no sensor key neither sets nor erases AFC presence",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // Weight alone. AFC's sensors have said nothing about this lane, and no
    // sensor reading is not a sensor reading of "empty".
    feed_afc_lane(*harness, "lane1", {{"weight", 612.0}});

    const auto unread = lane_sources(harness.lane(0));
    // Proof the translation ran, so the absence below is about the sensors.
    REQUIRE(unread.metered.has_value());
    CHECK_FALSE(unread.sensed.has_value());

    // The same lane once the sensors do speak. This half is what makes the
    // half above a guard rather than a backend that never reports presence.
    feed_afc_lane(*harness, "lane1", {{"prep", true}, {"status", "Loaded"}});

    const auto read = lane_sources(harness.lane(0));
    REQUIRE(read.sensed.has_value());
    REQUIRE(read.sensed->present.has_value());
    CHECK(*read.sensed->present == true);

    // Another weight-only frame. AFC's presence authority is per frame rather
    // than a latch, and a record is written whole, so filing an empty one here
    // would erase a live reading.
    feed_afc_lane(*harness, "lane1", {{"weight", 600.0}});

    const auto still = lane_sources(harness.lane(0));
    REQUIRE(still.sensed.has_value());
    REQUIRE(still.sensed->present.has_value());
    CHECK(*still.sensed->present == true);
}

TEST_CASE_METHOD(LVGLTestFixture, "each AFC lane accumulates its own identity",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane1",
                  {{"prep", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"}});

    // A second lane, saying nothing about any spool. One accumulator for the
    // backend instead of one per lane would hand it lane1's identity.
    feed_afc_lane(*harness, "lane2", {{"prep", false}, {"status", "None"}});

    const auto second = lane_sources(harness.lane(1));
    REQUIRE(second.vendor_cache.has_value());
    CHECK_FALSE(second.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(second.vendor_cache->material.has_value());
    CHECK_FALSE(second.vendor_cache->spool_name.has_value());

    const auto first = lane_sources(harness.lane(0));
    REQUIRE(first.vendor_cache.has_value());
    REQUIRE(first.vendor_cache->color_rgb.has_value());
    CHECK(*first.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(first.vendor_cache->material == "PETG");

    // The second lane's own spool, which must reach its own record and leave
    // the first lane's alone.
    feed_afc_lane(*harness, "lane2", {{"color", "#00AEFF"}, {"material", "PLA"}});

    const auto changed = lane_sources(harness.lane(1));
    REQUIRE(changed.vendor_cache.has_value());
    REQUIRE(changed.vendor_cache->color_rgb.has_value());
    CHECK(*changed.vendor_cache->color_rgb == 0x00AEFFu);
    CHECK(changed.vendor_cache->material == "PLA");

    const auto untouched = lane_sources(harness.lane(0));
    REQUIRE(untouched.vendor_cache.has_value());
    REQUIRE(untouched.vendor_cache->color_rgb.has_value());
    CHECK(*untouched.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(untouched.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a spool id AFC did not state is not filed as one",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    helix::ams::FilamentSlotOverride user;
    user.spoolman_id = 99;
    AfcTestAccess::overrides(*harness)[0] = user;

    // AFC unlinks the lane; the override re-supplies the user's id on the
    // merged slot, which is the precondition rather than the behaviour.
    feed_afc_lane(*harness, "lane1", {{"prep", true}, {"status", "Loaded"}, {"spool_id", nullptr}});
    REQUIRE(harness->get_slot_info(0).spoolman_id == 99);
    REQUIRE_FALSE(lane_sources(harness.lane(0)).vendor_cache->spoolman_id.has_value());

    // A spool_id that is neither an integer nor null states nothing. Reading
    // the merged slot here would file the user's 99 as AFC's own word.
    feed_afc_lane(*harness, "lane1", {{"spool_id", "junk"}});

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK_FALSE(lane.vendor_cache->spoolman_id.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's DB snapshot files on the same lane as its status frames",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // lane_data alone. No status frame has run, so every value below is this
    // parser's work and nothing else's.
    feed_afc_lane_data(*harness, {{"lane3",
                                   {{"color", "#ED2C2C"},
                                    {"material", "PETG"},
                                    {"name", "Galaxy Black"},
                                    {"vendor_name", "Kingroon"},
                                    {"spool_id", 7}}}});

    const auto lane = lane_sources(harness.lane(2));
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->material == "PETG");
    CHECK(lane.vendor_cache->spool_name == "Galaxy Black");
    CHECK(lane.vendor_cache->brand == "Kingroon");
    CHECK(lane.vendor_cache->spoolman_id == 7);

    // A database record is not a sensor and does not weigh anything.
    CHECK_FALSE(lane.sensed.has_value());
    CHECK_FALSE(lane.metered.has_value());

    // A value the snapshot carries but we cannot read is not the snapshot
    // saying the lane has no colour, so the record keeps the last one it could
    // read. Only an empty value removes it.
    feed_afc_lane_data(*harness, {{"lane3", {{"color", "#zzzzzz"}}}});

    const auto unreadable = lane_sources(harness.lane(2));
    REQUIRE(unreadable.vendor_cache.has_value());
    REQUIRE(unreadable.vendor_cache->color_rgb.has_value());
    CHECK(*unreadable.vendor_cache->color_rgb == 0xED2C2Cu);

    feed_afc_lane_data(*harness, {{"lane3", {{"color", ""}}}});
    CHECK_FALSE(lane_sources(harness.lane(2)).vendor_cache->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's two parsers accumulate into one account of a lane",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // The subscription carries colour, material and weight on every AFC
    // version; the filament name reaches pre-v1.2.0 firmware only through the
    // DB snapshot. Neither parser may narrow the lane to its own half.
    feed_afc_lane(*harness, "lane1",
                  {{"prep", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"weight", 612.0}});
    feed_afc_lane_data(*harness, {{"lane1", {{"name", "Galaxy Black"}}}});

    const auto joined = lane_sources(harness.lane(0));
    REQUIRE(joined.vendor_cache.has_value());
    REQUIRE(joined.vendor_cache->color_rgb.has_value());
    CHECK(*joined.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(joined.vendor_cache->material == "PETG");
    CHECK(joined.vendor_cache->spool_name == "Galaxy Black");

    // The snapshot said nothing about sensors or weight, so what the
    // subscription established still stands.
    REQUIRE(joined.sensed.has_value());
    CHECK(joined.sensed->present == true);
    REQUIRE(joined.metered.has_value());
    CHECK(*joined.metered->remaining_weight_g == Catch::Approx(612.0F));

    // An empty name in the snapshot is the clear AFC writes on eject, the same
    // answer the status path gives an empty filament_name.
    feed_afc_lane_data(*harness, {{"lane1", {{"name", ""}}}});

    const auto cleared = lane_sources(harness.lane(0));
    REQUIRE(cleared.vendor_cache.has_value());
    CHECK_FALSE(cleared.vendor_cache->spool_name.has_value());
    CHECK(cleared.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "an unreadable colour leaves AFC's record standing",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane2", {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}});
    REQUIRE(*lane_sources(harness.lane(1)).vendor_cache->color_rgb == 0xED2C2Cu);

    // "AFC published something we cannot read" and "AFC cleared the lane" are
    // different statements, and the record has to tell them apart or the lane
    // loses an identity nobody withdrew. The slot keeps its colour on screen;
    // the record keeps the same one.
    feed_afc_lane(*harness, "lane2", {{"color", "#zzzzzz"}});

    const auto unreadable = lane_sources(harness.lane(1));
    REQUIRE(unreadable.vendor_cache.has_value());
    REQUIRE(unreadable.vendor_cache->color_rgb.has_value());
    CHECK(*unreadable.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(harness->get_slot_info(1).color_rgb == 0xED2C2Cu);

    // The clear is the other statement, and it does remove the colour.
    feed_afc_lane(*harness, "lane2", {{"color", ""}});

    const auto cleared = lane_sources(harness.lane(1));
    REQUIRE(cleared.vendor_cache.has_value());
    CHECK_FALSE(cleared.vendor_cache->color_rgb.has_value());
    CHECK(harness->get_slot_info(1).color_rgb == helix::AMS_DEFAULT_SLOT_COLOR);
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare splits gate status from gate metadata",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 0, 1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "000000", "", "a4b2bc"})},
                        {"gate_material", nlohmann::json::array({"PETG", "PLA", "", "ABS"})},
                        {"gate_spool_id", nlohmann::json::array({7, 0, 0, 0})}});

    const auto gate0 = lane_sources(harness.lane(0));
    REQUIRE(gate0.sensed.has_value());
    REQUIRE(gate0.sensed->present.has_value());
    CHECK(*gate0.sensed->present == true);
    // The sensor's record carries presence and nothing else: what the gate map
    // remembers is a cache of a past declaration, not something a gate sensed.
    CHECK_FALSE(gate0.sensed->color_rgb.has_value());
    CHECK_FALSE(gate0.sensed->material.has_value());

    REQUIRE(gate0.vendor_cache.has_value());
    REQUIRE(gate0.vendor_cache->color_rgb.has_value());
    CHECK(*gate0.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(gate0.vendor_cache->material == "PETG");
    CHECK(gate0.vendor_cache->spoolman_id == 7);
    CHECK_FALSE(gate0.vendor_cache->present.has_value());
    // The gate map carries no weight at all.
    CHECK_FALSE(gate0.metered.has_value());

    const auto gate1 = lane_sources(harness.lane(1));
    REQUIRE(gate1.sensed.has_value());
    REQUIRE(gate1.sensed->present.has_value());
    CHECK(*gate1.sensed->present == false);
    // An empty gate still remembers what it last held. That memory is a cache,
    // and pure black in it is a colour like any other.
    REQUIRE(gate1.vendor_cache.has_value());
    REQUIRE(gate1.vendor_cache->color_rgb.has_value());
    CHECK(*gate1.vendor_cache->color_rgb == 0x000000u);
    // Happy Hare writes 0 for a gate with no spool, which is a clear rather
    // than a spool numbered zero.
    CHECK_FALSE(gate1.vendor_cache->spoolman_id.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare records no colour for a gate that reports none",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", ""})},
                        {"gate_material", nlohmann::json::array({"PETG", ""})}});

    const auto gate1 = lane_sources(harness.lane(1));
    // Proof the path ran: presence came through for the same gate.
    REQUIRE(gate1.sensed.has_value());
    REQUIRE(gate1.sensed->present.has_value());
    CHECK(*gate1.sensed->present == true);
    REQUIRE(gate1.vendor_cache.has_value());
    CHECK_FALSE(gate1.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(gate1.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a Happy Hare frame without metadata does not blank the cache",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "a4b2bc"})},
                        {"gate_material", nlohmann::json::array({"PETG", "ABS"})}});
    REQUIRE(lane_sources(harness.lane(0)).vendor_cache->material == "PETG");

    // A status-only delta states nothing about metadata, so it must not be
    // read as a gate that stopped reporting one.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({0, 1})}});

    const auto gate0 = lane_sources(harness.lane(0));
    REQUIRE(gate0.sensed->present.has_value());
    CHECK(*gate0.sensed->present == false);
    REQUIRE(gate0.vendor_cache.has_value());
    CHECK(gate0.vendor_cache->material == "PETG");
    REQUIRE(gate0.vendor_cache->color_rgb.has_value());
    CHECK(*gate0.vendor_cache->color_rgb == 0xED2C2Cu);
}

TEST_CASE_METHOD(LVGLTestFixture, "a partial Happy Hare delta narrows nothing it does not mention",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "a4b2bc"})},
                        {"gate_material", nlohmann::json::array({"PETG", "ABS"})},
                        {"gate_spool_id", nlohmann::json::array({7, 9})}});

    // MMU_GATE_MAP GATE=0 MATERIAL=PLA moves one array. Moonraker names only
    // what changed, so a record built from this frame alone would be a gate
    // whose colour and spool link had just vanished.
    feed_mmu(*harness, {{"gate_material", nlohmann::json::array({"PLA", "ABS"})}});

    const auto gate0 = lane_sources(harness.lane(0));
    REQUIRE(gate0.vendor_cache.has_value());
    CHECK(gate0.vendor_cache->material == "PLA");
    REQUIRE(gate0.vendor_cache->color_rgb.has_value());
    CHECK(*gate0.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(gate0.vendor_cache->spoolman_id == 7);
}

TEST_CASE_METHOD(LVGLTestFixture, "each Happy Hare gate accumulates its own identity",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // Gate 0 alone carries a spool. One accumulator for the backend instead of
    // one per gate would hand its identity to every other gate.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 0, 0})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "", ""})},
                        {"gate_material", nlohmann::json::array({"PETG", "", ""})},
                        {"gate_spool_id", nlohmann::json::array({7, 0, 0})}});

    const auto silent = lane_sources(harness.lane(1));
    REQUIRE(silent.vendor_cache.has_value());
    CHECK_FALSE(silent.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(silent.vendor_cache->material.has_value());
    CHECK_FALSE(silent.vendor_cache->spoolman_id.has_value());

    // Gate 1 gets its own spool; gate 0's record must not follow it.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"ed2c2c", "00aeff", ""})},
                        {"gate_material", nlohmann::json::array({"PETG", "PLA", ""})},
                        {"gate_spool_id", nlohmann::json::array({7, 12, 0})}});

    const auto changed = lane_sources(harness.lane(1));
    REQUIRE(changed.vendor_cache.has_value());
    REQUIRE(changed.vendor_cache->color_rgb.has_value());
    CHECK(*changed.vendor_cache->color_rgb == 0x00AEFFu);
    CHECK(changed.vendor_cache->material == "PLA");
    CHECK(changed.vendor_cache->spoolman_id == 12);

    const auto untouched = lane_sources(harness.lane(0));
    REQUIRE(untouched.vendor_cache.has_value());
    REQUIRE(untouched.vendor_cache->color_rgb.has_value());
    CHECK(*untouched.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(untouched.vendor_cache->material == "PETG");
    CHECK(untouched.vendor_cache->spoolman_id == 7);

    const auto quiet = lane_sources(harness.lane(2));
    REQUIRE(quiet.vendor_cache.has_value());
    CHECK_FALSE(quiet.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(quiet.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches Happy Hare's vendor-cache record",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // A user colour and material that disagree with what the gate map says.
    // SlotInfo persists across frames and apply_overrides() rewrites it in
    // place, so a translation reading that struct back would file the user's
    // own choice as something Happy Hare's gate map remembers.
    helix::ams::FilamentSlotOverride user;
    user.color_rgb = 0x00FF00u;
    user.color_set = true;
    user.user_locked_color = true;
    user.material = "ABS";
    {
        std::lock_guard<std::mutex> lock(HappyHareTestAccess::mutex(*harness));
        HappyHareTestAccess::overrides(*harness)[1] = user;
    }

    // gate_spool_id is what makes apply_overrides() run at all on this path.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"a4b2bc", "ed2c2c"})},
                        {"gate_material", nlohmann::json::array({"PLA", "PETG"})},
                        {"gate_spool_id", nlohmann::json::array({0, 0})}});

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot there is no laundering for the case to catch and
    // the assertions below would hold for the wrong reason.
    REQUIRE(harness->get_slot_info(1).color_rgb == 0x00FF00u);
    REQUIRE(harness->get_slot_info(1).material == "ABS");

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a gate Happy Hare calls unknown files no presence reading",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // Nothing on this lane may abort the case: the decisive claim is the last
    // phase, and a leading REQUIRE would report "no record filed" for a rule
    // whose actual failure is a stale reading surviving.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({-1, 1})}});

    const auto unread = lane_sources(harness.lane(0));
    // The record is filed even with nothing to say, so "the MMU does not know"
    // and "no translation ran" stay distinguishable.
    CHECK(unread.sensed.has_value());
    const bool unknown_gate_claims_a_reading =
        unread.sensed.has_value() && unread.sensed->present.has_value();
    CHECK_FALSE(unknown_gate_claims_a_reading);

    const auto known = lane_sources(harness.lane(1));
    REQUIRE(known.sensed.has_value());
    REQUIRE(known.sensed->present.has_value());
    CHECK(*known.sensed->present == true);

    // Positive contrast: the same gate reports a real status and the reading
    // lands, so the arm above is a guard and not a gate that never reports.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})}});
    const auto reporting = lane_sources(harness.lane(0));
    REQUIRE(reporting.sensed.has_value());
    REQUIRE(reporting.sensed->present.has_value());
    CHECK(*reporting.sensed->present == true);

    // Happy Hare withdrawing its word is news. One ingest replaces a source's
    // record whole, so filing nothing leaves the old reading standing: a
    // record whose present is unset is the only way to retract one.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({-1, 1})}});
    const auto withdrawn = lane_sources(harness.lane(0));
    const bool a_stale_reading_still_stands =
        withdrawn.sensed.has_value() && withdrawn.sensed->present.has_value();
    CHECK_FALSE(a_stale_reading_still_stands);
    CHECK(withdrawn.sensed.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an unreadable Happy Hare colour leaves the record standing",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "a4b2bc"})}});
    REQUIRE(*lane_sources(harness.lane(0)).vendor_cache->color_rgb == 0xED2C2Cu);

    // A value nothing can read is not a gate stating it has no colour, so the
    // last readable word stands in both the record and the slot.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"zzzzzz", "a4b2bc"})}});
    const auto unreadable = lane_sources(harness.lane(0));
    REQUIRE(unreadable.vendor_cache.has_value());
    REQUIRE(unreadable.vendor_cache->color_rgb.has_value());
    CHECK(*unreadable.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(harness->get_slot_info(0).color_rgb == 0xED2C2Cu);

    // Empty is Happy Hare wiping the gate, which is a statement and clears both.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"", "a4b2bc"})}});
    const auto cleared = lane_sources(harness.lane(0));
    REQUIRE(cleared.vendor_cache.has_value());
    CHECK_FALSE(cleared.vendor_cache->color_rgb.has_value());
    CHECK(harness->get_slot_info(0).color_rgb == helix::AMS_DEFAULT_SLOT_COLOR);
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare files the colour its numeric gate map states",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // gate_color_rgb wins over the hex strings, in both the shapes Happy Hare
    // and its emulator publish: packed integers and float triplets.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color_rgb", nlohmann::json::array({0xED2C2C, {0.0, 1.0, 0.0}})},
                        {"gate_color", nlohmann::json::array({"a4b2bc", "a4b2bc"})}});

    const auto packed = lane_sources(harness.lane(0));
    REQUIRE(packed.vendor_cache.has_value());
    REQUIRE(packed.vendor_cache->color_rgb.has_value());
    CHECK(*packed.vendor_cache->color_rgb == 0xED2C2Cu);

    const auto triplet = lane_sources(harness.lane(1));
    REQUIRE(triplet.vendor_cache.has_value());
    REQUIRE(triplet.vendor_cache->color_rgb.has_value());
    CHECK(*triplet.vendor_cache->color_rgb == 0x00FF00u);
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare files no reading for a gate it has no slot for",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // The gate count is fixed by the first gate_status frame, so a longer array
    // later names gates the backend never built a slot for.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})}});
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1, 1, 1})}});

    // Precondition, not the behaviour: without this the assertions below would
    // hold on a backend that had quietly grown the slots after all.
    REQUIRE(harness->get_slot_info(2).slot_index == -1);

    // Proof the path ran on the same frame: the gates that do exist reported.
    const auto real_gate = lane_sources(harness.lane(1));
    REQUIRE(real_gate.sensed.has_value());
    REQUIRE(real_gate.sensed->present.has_value());
    CHECK(*real_gate.sensed->present == true);

    CHECK_FALSE(lane_sources(harness.lane(2)).sensed.has_value());
    CHECK_FALSE(lane_sources(harness.lane(3)).sensed.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare's gate name is the spool's, and gate_name wins",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_name", nlohmann::json::array({"Galaxy Black", ""})},
                        {"gate_filament_name", nlohmann::json::array({"EMU Black", "EMU Blue"})}});

    // gate_name is the MMU's own field; gate_filament_name only fills a gap.
    const auto named = lane_sources(harness.lane(0));
    REQUIRE(named.vendor_cache.has_value());
    CHECK(named.vendor_cache->spool_name == "Galaxy Black");
    CHECK_FALSE(named.vendor_cache->color_name.has_value());

    const auto filled = lane_sources(harness.lane(1));
    REQUIRE(filled.vendor_cache.has_value());
    CHECK(filled.vendor_cache->spool_name == "EMU Blue");

    // A gate whose name is wiped stops carrying one, and the emulator's key
    // then fills the gap it left.
    feed_mmu(*harness, {{"gate_name", nlohmann::json::array({"", ""})}});
    const auto wiped = lane_sources(harness.lane(0));
    REQUIRE(wiped.vendor_cache.has_value());
    CHECK_FALSE(wiped.vendor_cache->spool_name.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a user's own name never decides Happy Hare's name precedence",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // SlotInfo::color_name is override-merged, so asking it which of the MMU's
    // two name keys won would let a person's edit answer for the firmware.
    helix::ams::FilamentSlotOverride user;
    user.color_name = "User Named It";
    {
        std::lock_guard<std::mutex> lock(HappyHareTestAccess::mutex(*harness));
        HappyHareTestAccess::overrides(*harness)[0] = user;
    }

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_spool_id", nlohmann::json::array({0, 0})},
                        {"gate_filament_name", nlohmann::json::array({"EMU Black", "EMU Blue"})}});

    // Precondition, not the behaviour under test: the override has to actually
    // win on the merged slot for there to be anything to launder.
    REQUIRE(harness->get_slot_info(0).color_name == "User Named It");

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->spool_name == "EMU Black");
}

// --- CFS ---------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "CFS splits bay occupancy from the box's tag memory",
                 "[lane][ingest][cfs]") {
    CfsHarness harness(nullptr, nullptr);

    feed_cfs_box(*harness,
                 flat_box(nlohmann::json::array({nlohmann::json{{"index", 0},
                                                                {"material", "PLA"},
                                                                {"brand", "Creality"},
                                                                {"name", "Hyper PLA"},
                                                                {"color", "#ED2C2C"},
                                                                {"present", true},
                                                                {"loaded", false},
                                                                {"spoolman_id", 7}},
                                                 nlohmann::json{{"index", 1},
                                                                {"material", "None"},
                                                                {"brand", "None"},
                                                                {"name", "None"},
                                                                {"color", "None"},
                                                                {"present", false},
                                                                {"loaded", false},
                                                                {"spoolman_id", nullptr}}})));

    const auto loaded_bay = lane_sources(harness.lane(0));
    REQUIRE(loaded_bay.sensed.has_value());
    REQUIRE(loaded_bay.sensed->present.has_value());
    CHECK(*loaded_bay.sensed->present == true);
    // Occupancy is the sensed record's whole business.
    CHECK_FALSE(loaded_bay.sensed->material.has_value());
    CHECK_FALSE(loaded_bay.sensed->color_rgb.has_value());

    REQUIRE(loaded_bay.vendor_cache.has_value());
    CHECK(loaded_bay.vendor_cache->material == "PLA");
    CHECK(loaded_bay.vendor_cache->brand == "Creality");
    REQUIRE(loaded_bay.vendor_cache->color_rgb.has_value());
    CHECK(*loaded_bay.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(loaded_bay.vendor_cache->spoolman_id == 7);
    // "Hyper PLA" is which product the bay holds, not what a spool is called.
    CHECK(loaded_bay.vendor_cache->product_name == "Hyper PLA");
    CHECK_FALSE(loaded_bay.vendor_cache->spool_name.has_value());
    // The box weighs nothing, so nothing meters this lane.
    CHECK_FALSE(loaded_bay.metered.has_value());

    // The firmware's "None" is its absence marker on every text field, and the
    // colour it pairs with it is not six hex digits either.
    const auto empty_bay = lane_sources(harness.lane(1));
    REQUIRE(empty_bay.sensed.has_value());
    REQUIRE(empty_bay.sensed->present.has_value());
    CHECK(*empty_bay.sensed->present == false);
    // Filed even with nothing in it, so "the box read no tag" and "no
    // translation ran" stay distinguishable.
    REQUIRE(empty_bay.vendor_cache.has_value());
    CHECK_FALSE(empty_bay.vendor_cache->material.has_value());
    CHECK_FALSE(empty_bay.vendor_cache->brand.has_value());
    CHECK_FALSE(empty_bay.vendor_cache->product_name.has_value());
    CHECK_FALSE(empty_bay.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(empty_bay.vendor_cache->spoolman_id.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches CFS's vendor-cache record",
                 "[lane][ingest][cfs]") {
    CfsHarness harness(nullptr, nullptr);

    // SlotInfo persists across frames and apply_overrides() rewrites it in
    // place at the convergence pass, so a translation reading that struct back
    // would file the user's own choice as something the box remembers.
    CfsTestAccess::seed_override(*harness, 1, user_colour_and_material());

    feed_cfs_box(*harness,
                 flat_box(nlohmann::json::array({nlohmann::json{{"index", 0}, {"present", false}},
                                                 nlohmann::json{{"index", 1},
                                                                {"material", "PETG"},
                                                                {"brand", "Creality"},
                                                                {"color", "#ED2C2C"},
                                                                {"present", true}}})));

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot there is no laundering for this case to catch and
    // both assertions below would hold for the wrong reason.
    const auto merged = harness->get_slot_info(1);
    REQUIRE(merged.color_rgb == 0x00FF00u);
    REQUIRE(merged.material == "ABS");

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "each CFS bay accumulates its own identity",
                 "[lane][ingest][cfs]") {
    CfsHarness harness(nullptr, nullptr);

    feed_cfs_box(
        *harness,
        flat_box(nlohmann::json::array(
            {nlohmann::json{
                 {"index", 0}, {"material", "PLA"}, {"color", "#111111"}, {"present", true}},
             nlohmann::json{
                 {"index", 1}, {"material", "None"}, {"color", "None"}, {"present", false}},
             nlohmann::json{
                 {"index", 2}, {"material", "PETG"}, {"color", "#333333"}, {"present", true}},
             nlohmann::json{
                 {"index", 3}, {"material", "ABS"}, {"color", "#444444"}, {"present", true}}})));

    // Values, not has_value(). Per-bay keys with every record filed on one lane
    // is the shape this defect actually takes, and only values catch it.
    const auto bay0 = lane_sources(harness.lane(0));
    REQUIRE(bay0.vendor_cache.has_value());
    CHECK(bay0.vendor_cache->material == "PLA");
    REQUIRE(bay0.vendor_cache->color_rgb.has_value());
    CHECK(*bay0.vendor_cache->color_rgb == 0x111111u);

    const auto bay1 = lane_sources(harness.lane(1));
    REQUIRE(bay1.vendor_cache.has_value());
    CHECK_FALSE(bay1.vendor_cache->material.has_value());
    CHECK_FALSE(bay1.vendor_cache->color_rgb.has_value());
    REQUIRE(bay1.sensed.has_value());
    CHECK(*bay1.sensed->present == false);

    const auto bay2 = lane_sources(harness.lane(2));
    REQUIRE(bay2.vendor_cache.has_value());
    CHECK(bay2.vendor_cache->material == "PETG");
    REQUIRE(bay2.vendor_cache->color_rgb.has_value());
    CHECK(*bay2.vendor_cache->color_rgb == 0x333333u);

    const auto bay3 = lane_sources(harness.lane(3));
    REQUIRE(bay3.vendor_cache.has_value());
    CHECK(bay3.vendor_cache->material == "ABS");
    REQUIRE(bay3.vendor_cache->color_rgb.has_value());
    CHECK(*bay3.vendor_cache->color_rgb == 0x444444u);
}

TEST_CASE_METHOD(LVGLTestFixture, "a CFS frame that names one unit leaves the others standing",
                 "[lane][ingest][cfs]") {
    CfsHarness harness(nullptr, nullptr);

    feed_cfs_box(*harness,
                 stock_box("T1", nlohmann::json{{"vender", nlohmann::json::array({"Creality"})},
                                                {"remain_len", nlohmann::json::array({"100"})},
                                                {"color_value", nlohmann::json::array({"0ED2C2C"})},
                                                {"material_type", nlohmann::json::array({"-1"})}}));

    const auto first_unit = lane_sources(harness.lane(0));
    REQUIRE(first_unit.sensed.has_value());
    REQUIRE(first_unit.sensed->present.has_value());
    CHECK(*first_unit.sensed->present == true);
    REQUIRE(first_unit.vendor_cache.has_value());
    REQUIRE(first_unit.vendor_cache->color_rgb.has_value());
    CHECK(*first_unit.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(first_unit.vendor_cache->brand == "Creality");

    // The next frame is about the second unit only. The box said nothing about
    // the first, and one ingest replaces a source's record whole, so filing an
    // empty record for the bays it did not mention would erase live readings.
    feed_cfs_box(*harness,
                 stock_box("T2", nlohmann::json{{"vender", nlohmann::json::array({"Xplorer"})},
                                                {"remain_len", nlohmann::json::array({"80"})},
                                                {"color_value", nlohmann::json::array({"0112233"})},
                                                {"material_type", nlohmann::json::array({"-1"})}}));

    // The first unit's lanes first, and with no REQUIRE above them: the failure
    // this case exists for is the second unit's readings landing on the first
    // unit's lanes, and an abort on "lane 4 is empty" would report the symptom
    // three assertions before the cause.
    const auto still = lane_sources(harness.lane(0));
    const bool the_first_unit_lost_its_reading =
        !still.sensed.has_value() || !still.sensed->present.has_value() || !*still.sensed->present;
    CHECK_FALSE(the_first_unit_lost_its_reading);
    const bool the_first_unit_took_the_second_units_colour =
        still.vendor_cache.has_value() && still.vendor_cache->color_rgb.has_value() &&
        *still.vendor_cache->color_rgb != 0xED2C2Cu;
    CHECK_FALSE(the_first_unit_took_the_second_units_colour);
    const bool the_first_unit_took_the_second_units_brand =
        still.vendor_cache.has_value() && still.vendor_cache->brand == "Xplorer";
    CHECK_FALSE(the_first_unit_took_the_second_units_brand);
    REQUIRE(still.vendor_cache.has_value());
    REQUIRE(still.vendor_cache->color_rgb.has_value());
    CHECK(*still.vendor_cache->color_rgb == 0xED2C2Cu);

    const auto second_unit = lane_sources(harness.lane(4));
    REQUIRE(second_unit.vendor_cache.has_value());
    REQUIRE(second_unit.vendor_cache->color_rgb.has_value());
    CHECK(*second_unit.vendor_cache->color_rgb == 0x112233u);
    CHECK(second_unit.vendor_cache->brand == "Xplorer");
}

TEST_CASE_METHOD(LVGLTestFixture, "CFS reads both its wire colours by the shared grammar",
                 "[lane][ingest][cfs]") {
    SECTION("a stock value with anything after the colour is no colour") {
        CfsHarness harness(nullptr, nullptr);
        // Creality's own spelling is a leading zero and six hex digits. A value
        // carrying more than that is refused whole rather than read up to its
        // first bad character, which would file a plausible wrong colour.
        feed_cfs_box(
            *harness,
            stock_box("T1", nlohmann::json{{"vender", nlohmann::json::array({"Creality"})},
                                           {"remain_len", nlohmann::json::array({"100"})},
                                           {"color_value", nlohmann::json::array({"0ED2C2CZZ"})},
                                           {"material_type", nlohmann::json::array({"-1"})}}));

        const auto lane = lane_sources(harness.lane(0));
        REQUIRE(lane.vendor_cache.has_value());
        CHECK_FALSE(lane.vendor_cache->color_rgb.has_value());
        // The bay is still occupied; only its colour is unreadable.
        REQUIRE(lane.sensed.has_value());
        CHECK(*lane.sensed->present == true);
    }

    SECTION("a stock value the old parse refused now reads") {
        CfsHarness harness(nullptr, nullptr);
        // Creality's leading-zero "0RRGGBB" is this function's own to strip;
        // what is left is read by the grammar every other lane-shaped producer
        // uses, so the ordinary web spellings mean here what they mean there.
        // Nothing validates this field before it reaches the parse.
        feed_cfs_box(
            *harness,
            stock_box("T1",
                      nlohmann::json{
                          {"vender", nlohmann::json::array({"Creality", "Creality", "Creality"})},
                          {"remain_len", nlohmann::json::array({"100", "100", "100"})},
                          {"color_value", nlohmann::json::array({"#ED2C2C", "#F00", "0ED2C2C"})},
                          {"material_type", nlohmann::json::array({"-1", "-1", "-1"})}}));

        const auto hashed = lane_sources(harness.lane(0));
        REQUIRE(hashed.vendor_cache.has_value());
        REQUIRE(hashed.vendor_cache->color_rgb.has_value());
        CHECK(*hashed.vendor_cache->color_rgb == 0xED2C2Cu);

        const auto three_digit = lane_sources(harness.lane(1));
        REQUIRE(three_digit.vendor_cache.has_value());
        REQUIRE(three_digit.vendor_cache->color_rgb.has_value());
        CHECK(*three_digit.vendor_cache->color_rgb == 0xFF0000u);

        // The Creality form this function still owns, so the delegation cannot
        // quietly take the leading-zero rule with it.
        const auto creality = lane_sources(harness.lane(2));
        REQUIRE(creality.vendor_cache.has_value());
        REQUIRE(creality.vendor_cache->color_rgb.has_value());
        CHECK(*creality.vendor_cache->color_rgb == 0xED2C2Cu);
    }

    SECTION("the flat schema reads every spelling the shared grammar accepts") {
        CfsHarness harness(nullptr, nullptr);
        feed_cfs_box(*harness,
                     flat_box(nlohmann::json::array(
                         {nlohmann::json{{"index", 0}, {"color", "#F00"}, {"present", true}},
                          nlohmann::json{{"index", 1}, {"color", "0xED2C2C"}, {"present", true}},
                          nlohmann::json{{"index", 2}, {"color", "112233FF"}, {"present", true}},
                          nlohmann::json{{"index", 3}, {"color", "nothex"}, {"present", true}}})));

        const auto three_digit = lane_sources(harness.lane(0));
        REQUIRE(three_digit.vendor_cache.has_value());
        REQUIRE(three_digit.vendor_cache->color_rgb.has_value());
        CHECK(*three_digit.vendor_cache->color_rgb == 0xFF0000u);

        const auto prefixed = lane_sources(harness.lane(1));
        REQUIRE(prefixed.vendor_cache.has_value());
        REQUIRE(prefixed.vendor_cache->color_rgb.has_value());
        CHECK(*prefixed.vendor_cache->color_rgb == 0xED2C2Cu);

        const auto with_alpha = lane_sources(harness.lane(2));
        REQUIRE(with_alpha.vendor_cache.has_value());
        REQUIRE(with_alpha.vendor_cache->color_rgb.has_value());
        CHECK(*with_alpha.vendor_cache->color_rgb == 0x112233u);

        const auto refused = lane_sources(harness.lane(3));
        REQUIRE(refused.vendor_cache.has_value());
        CHECK_FALSE(refused.vendor_cache->color_rgb.has_value());
    }
}

// --- ACE ---------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "ACE splits slot status from slot metadata",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    feed_ace(*harness,
             nlohmann::json{
                 {"status", "ready"},
                 {"slots", nlohmann::json::array({
                               nlohmann::json{{"status", "ready"},
                                              {"color", nlohmann::json::array({237, 44, 44})},
                                              {"type", "PETG"}},
                               nlohmann::json{{"status", "empty"},
                                              {"color", nlohmann::json::array({0, 0, 0})},
                                              {"type", ""}},
                           })},
             });

    const auto slot0 = lane_sources(harness.lane(0));
    REQUIRE(slot0.sensed.has_value());
    REQUIRE(slot0.sensed->present.has_value());
    CHECK(*slot0.sensed->present == true);
    CHECK_FALSE(slot0.sensed->color_rgb.has_value());
    REQUIRE(slot0.vendor_cache.has_value());
    REQUIRE(slot0.vendor_cache->color_rgb.has_value());
    CHECK(*slot0.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(slot0.vendor_cache->material == "PETG");
    // The hub weighs nothing and reads no tag, so no other source speaks.
    CHECK_FALSE(slot0.metered.has_value());
    CHECK_FALSE(slot0.spoolman.has_value());

    const auto slot1 = lane_sources(harness.lane(1));
    REQUIRE(slot1.sensed.has_value());
    REQUIRE(slot1.sensed->present.has_value());
    CHECK(*slot1.sensed->present == false);
    // ACE reports [0,0,0] for an empty slot. That is a real reading of black
    // from a hub that has no way to say "no reading", and the model records it
    // as the cache's word rather than inventing an absence.
    REQUIRE(slot1.vendor_cache.has_value());
    REQUIRE(slot1.vendor_cache->color_rgb.has_value());
    CHECK(*slot1.vendor_cache->color_rgb == 0x000000u);
    CHECK_FALSE(slot1.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches ACE's vendor-cache record",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    // SlotInfo persists across frames and apply_overrides() rewrites it in
    // place at the end of every slot iteration, so a translation reading that
    // struct back would file the user's own choice as the hub's memory.
    AceTestAccess::seed_override(*harness, 1, user_colour_and_material());

    feed_ace(*harness,
             nlohmann::json{
                 {"slots", nlohmann::json::array({
                               nlohmann::json{{"status", "empty"}},
                               nlohmann::json{{"status", "ready"},
                                              {"color", nlohmann::json::array({237, 44, 44})},
                                              {"type", "PETG"}},
                           })},
             });

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot there is no laundering for this case to catch and
    // both assertions below would hold for the wrong reason.
    const auto merged = harness->get_slot_info(1);
    REQUIRE(merged.color_rgb == 0x00FF00u);
    REQUIRE(merged.material == "ABS");

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->material == "PETG");

    // A second frame that states occupancy and nothing else. This is the phase
    // that can actually launder: the merge has been applied to the slot since
    // the last frame, so the struct now holds the user's colour and material
    // under keys this frame never mentioned, and a translation reading it back
    // would file both as the hub's own memory.
    feed_ace(*harness, nlohmann::json{{"slots", nlohmann::json::array({
                                                    nlohmann::json{{"status", "empty"}},
                                                    nlohmann::json{{"status", "ready"}},
                                                })}});

    // Precondition again: the override is still what the merged slot shows, so
    // there is still something for the record to launder.
    const auto after = harness->get_slot_info(1);
    REQUIRE(after.color_rgb == 0x00FF00u);
    REQUIRE(after.material == "ABS");

    const auto silent = lane_sources(harness.lane(1));
    REQUIRE(silent.vendor_cache.has_value());
    // Named separately from the two below so the failure carries its own
    // diagnosis: a record that merely narrowed wrongly and one that filed the
    // user's own edit as a hub reading are different defects.
    const bool the_users_colour_was_filed_as_the_hubs =
        silent.vendor_cache->color_rgb.has_value() && *silent.vendor_cache->color_rgb == 0x00FF00u;
    CHECK_FALSE(the_users_colour_was_filed_as_the_hubs);
    const bool the_users_material_was_filed_as_the_hubs =
        silent.vendor_cache->material.has_value() && *silent.vendor_cache->material == "ABS";
    CHECK_FALSE(the_users_material_was_filed_as_the_hubs);
    CHECK_FALSE(silent.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(silent.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a slot ACE states no status for neither sets nor erases presence",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    // Nothing on this lane may abort the case: the decisive claim is the last
    // phase, and a leading REQUIRE would report "no record filed" for a rule
    // whose actual failure is a stale reading being erased.
    feed_ace(*harness, nlohmann::json{{"slots", nlohmann::json::array({
                                                    nlohmann::json{{"type", "PLA"}},
                                                })}});

    const auto unread = lane_sources(harness.lane(0));
    // The cache record proves the translation ran, so the absence below is
    // about the hub's status key and not about an ingest that never happened.
    REQUIRE(unread.vendor_cache.has_value());
    CHECK(unread.vendor_cache->material == "PLA");
    CHECK_FALSE(unread.sensed.has_value());

    // The same slot once the hub does speak. This half is what makes the half
    // above a guard rather than a backend that never reports presence.
    feed_ace(*harness, nlohmann::json{{"slots", nlohmann::json::array({
                                                    nlohmann::json{{"status", "ready"}},
                                                })}});

    const auto read = lane_sources(harness.lane(0));
    REQUIRE(read.sensed.has_value());
    REQUIRE(read.sensed->present.has_value());
    CHECK(*read.sensed->present == true);

    // A frame that states no status again. The hub's presence authority is the
    // key, not a latch, so filing an empty record here would erase a live
    // reading instead of repeating it.
    feed_ace(*harness, nlohmann::json{{"slots", nlohmann::json::array({
                                                    nlohmann::json{{"type", "PLA"}},
                                                })}});

    const auto still = lane_sources(harness.lane(0));
    const bool a_live_reading_was_erased =
        !still.sensed.has_value() || !still.sensed->present.has_value();
    CHECK_FALSE(a_live_reading_was_erased);
    CHECK(still.sensed.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a status ACE does not recognise retracts its reading",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    feed_ace(*harness, nlohmann::json{{"slots", nlohmann::json::array({
                                                    nlohmann::json{{"status", "ready"}},
                                                })}});

    const auto read = lane_sources(harness.lane(0));
    REQUIRE(read.sensed.has_value());
    REQUIRE(read.sensed->present.has_value());
    CHECK(*read.sensed->present == true);

    // "unknown" is in ACE's own vocabulary and everything outside it lands in
    // the same place: the hub saying it does not know. That is news, and a
    // record whose field is unset is the only way to retract a reading.
    feed_ace(*harness, nlohmann::json{{"slots", nlohmann::json::array({
                                                    nlohmann::json{{"status", "unknown"}},
                                                })}});

    const auto withdrawn = lane_sources(harness.lane(0));
    const bool a_stale_reading_still_stands =
        withdrawn.sensed.has_value() && withdrawn.sensed->present.has_value();
    CHECK_FALSE(a_stale_reading_still_stands);
    CHECK(withdrawn.sensed.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "each ACE slot accumulates its own identity",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    feed_ace(*harness,
             nlohmann::json{
                 {"slots", nlohmann::json::array({
                               nlohmann::json{{"status", "ready"},
                                              {"color", nlohmann::json::array({17, 17, 17})},
                                              {"type", "PLA"}},
                               nlohmann::json{{"status", "empty"}},
                               nlohmann::json{{"status", "ready"},
                                              {"color", nlohmann::json::array({51, 51, 51})},
                                              {"type", "PETG"}},
                               nlohmann::json{{"status", "loaded"},
                                              {"color", nlohmann::json::array({68, 68, 68})},
                                              {"type", "ABS"}},
                           })},
             });

    // Values, not has_value(). Per-slot keys with every record filed on one
    // lane is the shape this defect actually takes, and only values catch it.
    const auto slot0 = lane_sources(harness.lane(0));
    REQUIRE(slot0.vendor_cache.has_value());
    CHECK(slot0.vendor_cache->material == "PLA");
    REQUIRE(slot0.vendor_cache->color_rgb.has_value());
    CHECK(*slot0.vendor_cache->color_rgb == 0x111111u);

    const auto slot1 = lane_sources(harness.lane(1));
    REQUIRE(slot1.vendor_cache.has_value());
    CHECK_FALSE(slot1.vendor_cache->material.has_value());
    CHECK_FALSE(slot1.vendor_cache->color_rgb.has_value());
    REQUIRE(slot1.sensed.has_value());
    CHECK(*slot1.sensed->present == false);

    const auto slot2 = lane_sources(harness.lane(2));
    REQUIRE(slot2.vendor_cache.has_value());
    CHECK(slot2.vendor_cache->material == "PETG");
    REQUIRE(slot2.vendor_cache->color_rgb.has_value());
    CHECK(*slot2.vendor_cache->color_rgb == 0x333333u);

    const auto slot3 = lane_sources(harness.lane(3));
    REQUIRE(slot3.vendor_cache.has_value());
    CHECK(slot3.vendor_cache->material == "ABS");
    REQUIRE(slot3.vendor_cache->color_rgb.has_value());
    CHECK(*slot3.vendor_cache->color_rgb == 0x444444u);
    REQUIRE(slot3.sensed.has_value());
    CHECK(*slot3.sensed->present == true);
}

TEST_CASE_METHOD(LVGLTestFixture, "an ACE colour that will not read is no colour",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    feed_ace(*harness,
             nlohmann::json{{"slots", nlohmann::json::array({
                                          nlohmann::json{{"status", "ready"}, {"color", "#ED2C2C"}},
                                      })}});

    const auto readable = lane_sources(harness.lane(0));
    REQUIRE(readable.vendor_cache.has_value());
    REQUIRE(readable.vendor_cache->color_rgb.has_value());
    CHECK(*readable.vendor_cache->color_rgb == 0xED2C2Cu);

    // Pure black is a colour ACE really reports, so "could not read this" must
    // not land on it. The record states no colour, and the bay keeps showing
    // the last one the hub did state.
    feed_ace(
        *harness,
        nlohmann::json{{"slots", nlohmann::json::array({
                                     nlohmann::json{{"status", "ready"}, {"color", "notahexvalue"}},
                                 })}});

    const auto unreadable = lane_sources(harness.lane(0));
    REQUIRE(unreadable.vendor_cache.has_value());
    CHECK_FALSE(unreadable.vendor_cache->color_rgb.has_value());
    CHECK(harness->get_slot_info(0).color_rgb == 0xED2C2Cu);

    // A short array carries no triplet and is the same answer.
    feed_ace(*harness,
             nlohmann::json{{"slots", nlohmann::json::array({
                                          nlohmann::json{{"status", "ready"},
                                                         {"color", nlohmann::json::array({17})}},
                                      })}});

    const auto truncated = lane_sources(harness.lane(0));
    REQUIRE(truncated.vendor_cache.has_value());
    CHECK_FALSE(truncated.vendor_cache->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE reads a colour string by the shared grammar",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    // The bridge's string form, in the spellings a hand-rolled hex read gets
    // wrong: a three-digit colour expands rather than landing on 0x000F00, an
    // alpha suffix is sliced off rather than shifting the colour out of range,
    // and a value with anything after the digits is refused rather than read up
    // to its first bad character.
    feed_ace(*harness, nlohmann::json{
                           {"slots", nlohmann::json::array({
                                         nlohmann::json{{"status", "ready"}, {"color", "#F00"}},
                                         nlohmann::json{{"status", "ready"}, {"color", "112233FF"}},
                                         nlohmann::json{{"status", "ready"}, {"color", "ED2C2CZZ"}},
                                         nlohmann::json{{"status", "ready"}, {"color", "0xED2C2C"}},
                                     })}});

    const auto three_digit = lane_sources(harness.lane(0));
    REQUIRE(three_digit.vendor_cache.has_value());
    REQUIRE(three_digit.vendor_cache->color_rgb.has_value());
    CHECK(*three_digit.vendor_cache->color_rgb == 0xFF0000u);
    CHECK(harness->get_slot_info(0).color_rgb == 0xFF0000u);

    const auto with_alpha = lane_sources(harness.lane(1));
    REQUIRE(with_alpha.vendor_cache.has_value());
    REQUIRE(with_alpha.vendor_cache->color_rgb.has_value());
    CHECK(*with_alpha.vendor_cache->color_rgb == 0x112233u);

    const auto trailing_junk = lane_sources(harness.lane(2));
    REQUIRE(trailing_junk.vendor_cache.has_value());
    CHECK_FALSE(trailing_junk.vendor_cache->color_rgb.has_value());

    const auto prefixed = lane_sources(harness.lane(3));
    REQUIRE(prefixed.vendor_cache.has_value());
    REQUIRE(prefixed.vendor_cache->color_rgb.has_value());
    CHECK(*prefixed.vendor_cache->color_rgb == 0xED2C2Cu);
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE's REST bridge files on the same lanes as its subscription",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    // The bridge is a second parser with its own key ladder: `material` before
    // `type`, and a hex string rather than a triplet. It polls the same hub, so
    // it files the same account on the same lanes.
    AceTestAccess::parse_slots(*harness, nlohmann::json{
                                             {"slots", nlohmann::json::array({
                                                           nlohmann::json{{"status", "ready"},
                                                                          {"color", "#ED2C2C"},
                                                                          {"material", "PETG"},
                                                                          {"type", "PLA"}},
                                                           nlohmann::json{{"status", "empty"}},
                                                       })},
                                         });

    const auto slot0 = lane_sources(harness.lane(0));
    REQUIRE(slot0.sensed.has_value());
    REQUIRE(slot0.sensed->present.has_value());
    CHECK(*slot0.sensed->present == true);
    REQUIRE(slot0.vendor_cache.has_value());
    REQUIRE(slot0.vendor_cache->color_rgb.has_value());
    CHECK(*slot0.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(slot0.vendor_cache->material == "PETG");

    const auto slot1 = lane_sources(harness.lane(1));
    REQUIRE(slot1.sensed.has_value());
    REQUIRE(slot1.sensed->present.has_value());
    CHECK(*slot1.sensed->present == false);
    REQUIRE(slot1.vendor_cache.has_value());
    CHECK_FALSE(slot1.vendor_cache->material.has_value());
    CHECK_FALSE(slot1.vendor_cache->color_rgb.has_value());
}

// ============================================================================
// Snapmaker
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Snapmaker's RFID product line is not a spool name",
                 "[lane][ingest][snapmaker]") {
    SnapmakerHarness harness(nullptr, nullptr);

    feed_filament_detect(*harness,
                         nlohmann::json{
                             {"state", nlohmann::json::array({1, 0, 0, 0})},
                             {"info", nlohmann::json::array({nlohmann::json{
                                          {"MAIN_TYPE", "PLA"},
                                          {"SUB_TYPE", "Silk"},
                                          {"MANUFACTURER", "Snapmaker"},
                                          {"ARGB_COLOR", 0xFFED2C2C},
                                          {"WEIGHT", 1000},
                                          {"CARD_UID", nlohmann::json::array({144, 32, 196, 2})},
                                      }})},
                         });

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);

    REQUIRE(lane.vendor_cache.has_value());
    // MAIN_TYPE is the material and keeps the field.
    CHECK(lane.vendor_cache->material == "PLA");
    // SUB_TYPE names the product line inside that material, so it is the
    // branded product rather than a second spelling of the material. The
    // SlotInfo this same parse writes spells it spool_name; the record
    // deliberately does not agree with it.
    CHECK(lane.vendor_cache->product_name == "Silk");
    CHECK_FALSE(lane.vendor_cache->spool_name.has_value());
    CHECK(harness->get_slot_info(0).spool_name == "Silk");

    CHECK(lane.vendor_cache->brand == "Snapmaker");
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    REQUIRE(lane.vendor_cache->total_weight_g.has_value());
    CHECK(*lane.vendor_cache->total_weight_g == 1000.0F);

    // A channel the same frame reports empty. The info array carried one entry,
    // so nothing declared anything here.
    const auto empty = lane_sources(harness.lane(1));
    REQUIRE(empty.sensed.has_value());
    REQUIRE(empty.sensed->present.has_value());
    CHECK(*empty.sensed->present == false);
    CHECK_FALSE(empty.vendor_cache.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "Snapmaker's NONE tag states nothing about identity",
                 "[lane][ingest][snapmaker]") {
    SnapmakerHarness harness(nullptr, nullptr);

    feed_filament_detect(*harness,
                         nlohmann::json{
                             {"state", nlohmann::json::array({1, 1, 0, 0})},
                             {"info", nlohmann::json::array({
                                          nlohmann::json{{"MAIN_TYPE", "NONE"}},
                                          // The positive control, and it has to live in this same
                                          // info array. The Sensed record cannot play that part:
                                          // it is filed by the state loop, over a different wire
                                          // key, so it stays green with the whole identity ingest
                                          // deleted. A neighbour that DOES declare is what makes
                                          // lane 0's silence a decision the loop took.
                                          nlohmann::json{{"MAIN_TYPE", "PLA"}},
                                      })},
                         });

    const auto lane = lane_sources(harness.lane(0));
    // Presence still came through: the channel senses filament even with no
    // readable tag, which is the whole shape of "sensed, not declared".
    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);
    CHECK_FALSE(lane.vendor_cache.has_value());

    const auto control = lane_sources(harness.lane(1));
    REQUIRE(control.vendor_cache.has_value());
    CHECK(control.vendor_cache->material == "PLA");
}

TEST_CASE_METHOD(LVGLTestFixture, "Snapmaker files no colour for a tag that carried none",
                 "[lane][ingest][snapmaker]") {
    SnapmakerHarness harness(nullptr, nullptr);

    // A tag with an identity but no ARGB_COLOR. SnapmakerRfidInfo::color_rgb
    // rests on AMS_DEFAULT_SLOT_COLOR, which is that struct's "no reading" and
    // not a grey a vendor printed on a spool.
    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1, 1})},
                                       {"info", nlohmann::json::array({
                                                    nlohmann::json{
                                                        {"MAIN_TYPE", "PETG"},
                                                        {"SUB_TYPE", "NONE"},
                                                    },
                                                    // A tag the reader answered for but that
                                                    // named nothing at all. Every field rests
                                                    // on its struct default, so the record
                                                    // carries none of them.
                                                    nlohmann::json{{"BED_TEMP", 60}},
                                                })},
                                   });

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->material == "PETG");
    CHECK_FALSE(lane.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(lane.vendor_cache->product_name.has_value());
    CHECK_FALSE(lane.vendor_cache->total_weight_g.has_value());
    CHECK_FALSE(lane.vendor_cache->brand.has_value());
    // The sentinel still reaches SlotInfo, so the record's silence is the only
    // place the difference between "grey" and "no reading" survives.
    CHECK(harness->get_slot_info(0).color_rgb == helix::AMS_DEFAULT_SLOT_COLOR);

    const auto silent = lane_sources(harness.lane(1));
    REQUIRE(silent.vendor_cache.has_value());
    CHECK_FALSE(silent.vendor_cache->material.has_value());
    CHECK_FALSE(silent.vendor_cache->brand.has_value());
    CHECK_FALSE(silent.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(silent.vendor_cache->product_name.has_value());
    CHECK_FALSE(silent.vendor_cache->total_weight_g.has_value());

    // Pure black is a colour a vendor can print, and the same guard keeps it.
    feed_filament_detect(*harness, nlohmann::json{
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PETG"},
                                                    {"ARGB_COLOR", 0xFF000000},
                                                }})},
                                   });

    const auto black = lane_sources(harness.lane(0));
    REQUIRE(black.vendor_cache.has_value());
    REQUIRE(black.vendor_cache->color_rgb.has_value());
    CHECK(*black.vendor_cache->color_rgb == 0x000000u);
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches Snapmaker's vendor-cache record",
                 "[lane][ingest][snapmaker]") {
    SnapmakerHarness harness(nullptr, nullptr);

    const auto tag = nlohmann::json{
        {"state", nlohmann::json::array({1})},
        {"info", nlohmann::json::array({nlohmann::json{
                     {"MAIN_TYPE", "PLA"},
                     {"ARGB_COLOR", 0xFFED2C2C},
                 }})},
    };

    // Frame one establishes firmware truth on the lane.
    feed_filament_detect(*harness, tag);
    REQUIRE(harness->get_slot_info(0).color_rgb == 0xED2C2Cu);

    // The user overrides it. apply_overrides runs at the tail of every frame
    // and rewrites the persistent SlotInfo in place, so from here the merged
    // struct carries the user's colour and material rather than the tag's.
    //
    // Both locks are needed for that: this backend mirrors firmware truth back
    // into the record on the OverwriteAlways policy, which replaces an unlocked
    // field with what the tag says before apply_overrides ever reads it.
    auto user = user_colour_and_material();
    user.user_locked_material = true;
    SnapmakerTestAccess::seed_override(*harness, 0, user);

    // Frame two re-reads the same tag. The record must say what the tag says.
    feed_filament_detect(*harness, tag);

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot there is no laundering for the case to catch and
    // the assertions below would hold for the wrong reason.
    REQUIRE(harness->get_slot_info(0).color_rgb == 0x00FF00u);
    REQUIRE(harness->get_slot_info(0).material == "ABS");

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->material == "PLA");
}

// ============================================================================
// Tool changer
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "a tool changer senses docking and declares nothing",
                 "[lane][ingest][toolchanger]") {
    ToolChangerHarness harness(nullptr, nullptr);

    harness->set_discovered_tools({"T0", "T1"});

    // AmsState hands a backend its tool list BEFORE add_backend() stamps an
    // index, so the records initialize_tools() filed land on no lane in
    // production. Drop them and drive a real status frame, which is the path a
    // machine takes.
    helix::ams::reset_lane_sources();
    REQUIRE_FALSE(lane_sources(harness.lane(0)).sensed.has_value());
    feed_toolchanger(*harness,
                     nlohmann::json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}}});

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);

    // klipper-toolchanger reports whether a tool is docked and nothing about
    // what it holds. initialize_tools() puts the tool's own name in
    // SlotInfo::spool_name and rests the colour on the sentinel; neither is a
    // reading, so this backend files no identity record at all. Both are
    // asserted here because they are what a read-back of the merged struct
    // would have filed.
    CHECK(harness->get_slot_info(0).spool_name == "T0");
    CHECK(harness->get_slot_info(0).color_rgb == helix::AMS_DEFAULT_SLOT_COLOR);
    CHECK_FALSE(lane.vendor_cache.has_value());

    const auto second = lane_sources(harness.lane(1));
    REQUIRE(second.sensed.has_value());
    REQUIRE(second.sensed->present.has_value());
    CHECK(*second.sensed->present == true);
    CHECK_FALSE(second.vendor_cache.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a vacated dock retracts a tool changer's presence reading",
                 "[lane][ingest][toolchanger]") {
    ToolChangerHarness harness(nullptr, nullptr);
    harness->set_discovered_tools({"T0", "T1", "T2"});
    harness->set_tool_sensor(helix::toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    // AmsState hands a backend its tool list BEFORE add_backend() stamps an
    // index, so nothing the resulting initialize_tools() files has a lane to
    // land on in production. Dropping those records leaves this case resting on
    // the status path alone, which is the one that runs with an index.
    helix::ams::reset_lane_sources();
    REQUIRE_FALSE(lane_sources(harness.lane(0)).sensed.has_value());

    // T1 is on the head and T2's dock reads vacant, which means that hot end
    // has been taken out of the machine.
    feed_toolchanger(*harness,
                     nlohmann::json{{"medusahc",
                                     {{"operation", "idle"},
                                      {"current_tool", 1},
                                      {"sensors", {{"e", 1}, {"t0", 1}, {"t1", 0}, {"t2", 0}}}}}});

    REQUIRE(harness->get_slot_info(2).status == helix::SlotStatus::EMPTY);

    const auto docked = lane_sources(harness.lane(0));
    REQUIRE(docked.sensed.has_value());
    REQUIRE(docked.sensed->present.has_value());
    CHECK(*docked.sensed->present == true);

    const auto carriage = lane_sources(harness.lane(1));
    REQUIRE(carriage.sensed.has_value());
    REQUIRE(carriage.sensed->present.has_value());
    CHECK(*carriage.sensed->present == true);

    const auto vacated = lane_sources(harness.lane(2));
    REQUIRE(vacated.sensed.has_value());
    REQUIRE(vacated.sensed->present.has_value());
    CHECK(*vacated.sensed->present == false);
    CHECK_FALSE(vacated.vendor_cache.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never becomes a tool changer's vendor reading",
                 "[lane][ingest][toolchanger]") {
    ToolChangerHarness harness(nullptr, nullptr);
    harness->set_discovered_tools({"T0", "T1"});

    // On this backend the override store is the ONLY source of filament
    // identity, so every identity field on the merged slot is the user's own
    // statement and there is nothing else for a read-back to pick up.
    ToolChangerTestAccess::seed_override(*harness, 0, user_colour_and_material());

    // The production laundering shape, which a second set_discovered_tools()
    // does not reach: a real status frame, where refresh_slot_statuses_locked
    // files the reading and the tail re-layer runs apply_overrides after it.
    helix::ams::reset_lane_sources();
    feed_toolchanger(*harness,
                     nlohmann::json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}}});

    REQUIRE(harness->get_slot_info(0).color_rgb == 0x00FF00u);
    REQUIRE(harness->get_slot_info(0).material == "ABS");

    const auto lane = lane_sources(harness.lane(0));
    // The presence record is the proof that the translation ran on this frame.
    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);
    CHECK_FALSE(lane.vendor_cache.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "Snapmaker reads NONE as a tag saying nothing, not as a brand",
                 "[lane][ingest][snapmaker]") {
    SnapmakerHarness harness(nullptr, nullptr);

    // "NONE" is the firmware's spelling for an unset string, and it reaches the
    // brand through two keys: MANUFACTURER is preferred and VENDOR is the
    // fallback, so each has to be refused on its own.
    feed_filament_detect(
        *harness, nlohmann::json{
                      {"state", nlohmann::json::array({1, 1, 1})},
                      {"info", nlohmann::json::array({
                                   nlohmann::json{{"MAIN_TYPE", "PLA"}, {"MANUFACTURER", "NONE"}},
                                   nlohmann::json{{"MAIN_TYPE", "PLA"}, {"VENDOR", "NONE"}},
                                   nlohmann::json{{"MAIN_TYPE", "PLA"}, {"VENDOR", "Polymaker"}},
                               })},
                  });

    const auto manufacturer_none = lane_sources(harness.lane(0));
    REQUIRE(manufacturer_none.vendor_cache.has_value());
    CHECK_FALSE(manufacturer_none.vendor_cache->brand.has_value());
    // The material on the same record is the control: this entry was read and
    // filed, and the brand guard is what dropped the one field.
    CHECK(manufacturer_none.vendor_cache->material == "PLA");

    const auto vendor_none = lane_sources(harness.lane(1));
    REQUIRE(vendor_none.vendor_cache.has_value());
    CHECK_FALSE(vendor_none.vendor_cache->brand.has_value());
    CHECK(vendor_none.vendor_cache->material == "PLA");

    // The fallback key naming a real vendor still reaches the record.
    const auto vendor_real = lane_sources(harness.lane(2));
    REQUIRE(vendor_real.vendor_cache.has_value());
    CHECK(vendor_real.vendor_cache->brand == "Polymaker");
}

TEST_CASE_METHOD(LVGLTestFixture, "a Snapmaker tag that stops naming a vendor retracts it",
                 "[lane][ingest][snapmaker]") {
    SnapmakerHarness harness(nullptr, nullptr);

    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PLA"},
                                                    {"MANUFACTURER", "Snapmaker"},
                                                    {"SUB_TYPE", "Silk"},
                                                }})},
                                   });

    const auto stated = lane_sources(harness.lane(0));
    REQUIRE(stated.vendor_cache.has_value());
    CHECK(stated.vendor_cache->brand == "Snapmaker");
    CHECK(stated.vendor_cache->product_name == "Silk");
    REQUIRE(harness->get_slot_info(0).brand == "Snapmaker");
    REQUIRE(harness->get_slot_info(0).spool_name == "Silk");

    // The firmware spelling "NONE". The record retracts, because whole-record
    // replacement means it states what THIS read said. SlotInfo keeps the old
    // value, because its guards test the literal and it has no way to say "no
    // longer stated". This is the one input where the two layers disagree, and
    // the disagreement is deliberate.
    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PLA"},
                                                    {"MANUFACTURER", "NONE"},
                                                    {"SUB_TYPE", "NONE"},
                                                }})},
                                   });

    const auto spelled_none = lane_sources(harness.lane(0));
    REQUIRE(spelled_none.vendor_cache.has_value());
    // Material is the control: this read did reach the ingest.
    CHECK(spelled_none.vendor_cache->material == "PLA");
    CHECK_FALSE(spelled_none.vendor_cache->brand.has_value());
    CHECK_FALSE(spelled_none.vendor_cache->product_name.has_value());
    CHECK(harness->get_slot_info(0).brand == "Snapmaker");
    CHECK(harness->get_slot_info(0).spool_name == "Silk");

    // An ABSENT key is the other spelling of the same silence, and here the two
    // layers agree: both guards let the empty string through to SlotInfo, so it
    // blanks rather than keeping.
    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PLA"},
                                                }})},
                                   });

    const auto absent = lane_sources(harness.lane(0));
    REQUIRE(absent.vendor_cache.has_value());
    CHECK(absent.vendor_cache->material == "PLA");
    CHECK_FALSE(absent.vendor_cache->brand.has_value());
    CHECK_FALSE(absent.vendor_cache->product_name.has_value());
    CHECK(harness->get_slot_info(0).brand.empty());
    CHECK(harness->get_slot_info(0).spool_name.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "Snapmaker's print_task_config writes identity and observes none",
                 "[lane][ingest][snapmaker]") {
    SnapmakerHarness harness(nullptr, nullptr);

    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PLA"},
                                                    {"MANUFACTURER", "Snapmaker"},
                                                    {"ARGB_COLOR", 0xFFED2C2C},
                                                }})},
                                   });

    // SET_PRINT_FILAMENT_CONFIG takes these as gcode parameters, so whoever
    // sent that command set them: the machine's screen, a slicer, a console, or
    // this backend's own write-back, which firmware mirrors into this struct.
    // It is a write surface and files nothing.
    feed_print_task_config(*harness,
                           nlohmann::json{
                               {"filament_type", nlohmann::json::array({"PETG"})},
                               {"filament_vendor", nlohmann::json::array({"SomebodyElse"})},
                               {"filament_color_rgba", nlohmann::json::array({"00FF00FF"})},
                           });

    // The control: the parse ran and took every field onto the merged struct.
    REQUIRE(harness->get_slot_info(0).material == "PETG");
    REQUIRE(harness->get_slot_info(0).brand == "SomebodyElse");
    REQUIRE(harness->get_slot_info(0).color_rgb == 0x00FF00u);

    // The record still holds the tag read, which is the only thing on this
    // backend that was measured rather than declared.
    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->material == "PLA");
    CHECK(lane.vendor_cache->brand == "Snapmaker");
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
}

TEST_CASE_METHOD(LVGLTestFixture, "Snapmaker's own write-back does not return as a vendor reading",
                 "[lane][ingest][snapmaker]") {
    // The write-back needs a real API behind it: /printer/filament_detect/set
    // is only POSTed when one is attached, and that POST is what the parse has
    // to recognise on the way back.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    SnapmakerHarness harness(&api, nullptr);

    const auto tag_uid = nlohmann::json::array({144, 32, 196, 2});

    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PLA"},
                                                    {"MANUFACTURER", "Snapmaker"},
                                                    {"SUB_TYPE", "Silk"},
                                                    {"ARGB_COLOR", 0xFFED2C2C},
                                                    {"CARD_UID", tag_uid},
                                                }})},
                                   });

    const auto read = lane_sources(harness.lane(0));
    REQUIRE(read.vendor_cache.has_value());
    REQUIRE(read.vendor_cache->color_rgb.has_value());
    CHECK(*read.vendor_cache->color_rgb == 0xED2C2Cu);

    // The user edits through the production path, which POSTs VENDOR /
    // MAIN_TYPE / SUB_TYPE / RGB_1 into filament_detect.info.
    auto edit = harness->get_slot_info(0);
    edit.brand = "Polymaker";
    edit.material = "PETG";
    edit.spool_name = "Matte";
    edit.color_rgb = 0x00FF00u;
    REQUIRE(harness->set_slot_info(0, edit, /*persist=*/true).success());
    REQUIRE(api.rest_mock().mock_get_post_history().size() == 1);
    REQUIRE(api.rest_mock().mock_get_post_history()[0].endpoint == "/printer/filament_detect/set");

    // Firmware reports the write back through the same object the tag uses,
    // with the same key spellings, on the same physical spool. WEIGHT is the
    // control: nothing POSTs it, so it is a genuine reading and must still be
    // filed while the four echoed fields are withheld.
    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PETG"},
                                                    {"MANUFACTURER", "Polymaker"},
                                                    {"SUB_TYPE", "Matte"},
                                                    {"ARGB_COLOR", 0xFF00FF00},
                                                    {"WEIGHT", 1000},
                                                    {"CARD_UID", tag_uid},
                                                }})},
                                   });

    const auto echoed = lane_sources(harness.lane(0));
    REQUIRE(echoed.vendor_cache.has_value());
    REQUIRE(echoed.vendor_cache->total_weight_g.has_value());
    CHECK(*echoed.vendor_cache->total_weight_g == 1000.0F);
    CHECK_FALSE(echoed.vendor_cache->material.has_value());
    CHECK_FALSE(echoed.vendor_cache->brand.has_value());
    CHECK_FALSE(echoed.vendor_cache->product_name.has_value());
    CHECK_FALSE(echoed.vendor_cache->color_rgb.has_value());

    // The same tag, re-read, reporting what is physically printed on it rather
    // than what we wrote. The UID has not moved, so the echo is still
    // outstanding, but these values are not the ones we sent and the guard
    // must let every one of them through. Suppressing on the mere existence of
    // an outstanding write, instead of on an exact value match, would lose a
    // genuine reading here.
    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PLA"},
                                                    {"MANUFACTURER", "Snapmaker"},
                                                    {"SUB_TYPE", "Silk"},
                                                    {"ARGB_COLOR", 0xFFED2C2C},
                                                    {"CARD_UID", tag_uid},
                                                }})},
                                   });

    const auto reasserted = lane_sources(harness.lane(0));
    REQUIRE(reasserted.vendor_cache.has_value());
    CHECK(reasserted.vendor_cache->material == "PLA");
    CHECK(reasserted.vendor_cache->brand == "Snapmaker");
    CHECK(reasserted.vendor_cache->product_name == "Silk");
    REQUIRE(reasserted.vendor_cache->color_rgb.has_value());
    CHECK(*reasserted.vendor_cache->color_rgb == 0xED2C2Cu);

    // A different CARD_UID is a different physical spool, so the same values
    // are now a tag stating them rather than firmware repeating us.
    feed_filament_detect(*harness,
                         nlohmann::json{
                             {"state", nlohmann::json::array({1})},
                             {"info", nlohmann::json::array({nlohmann::json{
                                          {"MAIN_TYPE", "PETG"},
                                          {"MANUFACTURER", "Polymaker"},
                                          {"SUB_TYPE", "Matte"},
                                          {"ARGB_COLOR", 0xFF00FF00},
                                          {"CARD_UID", nlohmann::json::array({9, 9, 9, 9})},
                                      }})},
                         });

    const auto swapped = lane_sources(harness.lane(0));
    REQUIRE(swapped.vendor_cache.has_value());
    CHECK(swapped.vendor_cache->material == "PETG");
    CHECK(swapped.vendor_cache->brand == "Polymaker");
    CHECK(swapped.vendor_cache->product_name == "Matte");
    REQUIRE(swapped.vendor_cache->color_rgb.has_value());
    CHECK(*swapped.vendor_cache->color_rgb == 0x00FF00u);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Snapmaker withholds only the fields the user moved, not the whole write-back",
                 "[lane][ingest][snapmaker]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    SnapmakerHarness harness(&api, nullptr);

    const auto tag_uid = nlohmann::json::array({144, 32, 196, 2});
    const auto tag = nlohmann::json{
        {"state", nlohmann::json::array({1})},
        {"info", nlohmann::json::array({nlohmann::json{
                     {"MAIN_TYPE", "PLA"},
                     {"MANUFACTURER", "Snapmaker"},
                     {"SUB_TYPE", "Silk"},
                     {"ARGB_COLOR", 0xFFED2C2C},
                     {"CARD_UID", tag_uid},
                 }})},
    };

    feed_filament_detect(*harness, tag);

    // The user changes the COLOUR and nothing else. commit_slot_edit files a
    // strict delta, so LocalUser will hold the colour alone.
    auto edit = harness->get_slot_info(0);
    REQUIRE(edit.brand == "Snapmaker");
    REQUIRE(edit.material == "PLA");
    REQUIRE(edit.spool_name == "Silk");
    edit.color_rgb = 0x00FF00u;
    REQUIRE(harness->set_slot_info(0, edit, /*persist=*/true).success());

    // set_slot_info POSTs the whole merged struct, so VENDOR / MAIN_TYPE /
    // SUB_TYPE went out carrying the tag's own strings.
    const auto history = api.rest_mock().mock_get_post_history();
    REQUIRE(history.size() == 1);
    const nlohmann::json body = history[0].body["info"];
    REQUIRE(body["VENDOR"].get<std::string>() == "Snapmaker");
    REQUIRE(body["MAIN_TYPE"].get<std::string>() == "PLA");
    REQUIRE(body["SUB_TYPE"].get<std::string>() == "Silk");

    // Firmware reports all four back. Only the colour is the user's
    // declaration; the other three are the tag's own values making a round
    // trip, and no other source holds them, so they must be filed.
    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PLA"},
                                                    {"MANUFACTURER", "Snapmaker"},
                                                    {"SUB_TYPE", "Silk"},
                                                    {"ARGB_COLOR", 0xFF00FF00},
                                                    {"CARD_UID", tag_uid},
                                                }})},
                                   });

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->material == "PLA");
    CHECK(lane.vendor_cache->brand == "Snapmaker");
    CHECK(lane.vendor_cache->product_name == "Silk");
    CHECK_FALSE(lane.vendor_cache->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a Spoolman link declares the binding, not the values it carries",
                 "[lane][ingest][snapmaker]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    SnapmakerHarness harness(&api, nullptr);

    const auto tag_uid = nlohmann::json::array({144, 32, 196, 2});
    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PLA"},
                                                    {"MANUFACTURER", "Snapmaker"},
                                                    {"ARGB_COLOR", 0xFFED2C2C},
                                                    {"CARD_UID", tag_uid},
                                                }})},
                                   });

    // Linking a spool carries the spool profile's brand, material and colour
    // into the same commit. Nobody chose those, so user_edit_observation files
    // the binding alone, and the write-back guard must not treat them as
    // declarations either. A per-field delta recomputed beside it would.
    const auto original = harness->get_slot_info(0);
    auto edit = original;
    edit.spoolman_id = 42;
    edit.brand = "Polymaker";
    edit.material = "PETG";
    edit.color_rgb = 0x00FF00u;
    REQUIRE(harness->set_slot_info(0, edit, /*persist=*/true).success());

    // The user's own record, filed the way AmsState::commit_slot_edit files it.
    helix::ams::commit_slot_edit(harness.lane(0),
                                 helix::ams::user_edit_observation(original, edit));

    const auto declared = lane_sources(harness.lane(0));
    REQUIRE(declared.local_user.has_value());
    REQUIRE(declared.local_user->spoolman_id.has_value());
    CHECK(*declared.local_user->spoolman_id == 42);
    CHECK_FALSE(declared.local_user->brand.has_value());
    CHECK_FALSE(declared.local_user->material.has_value());
    CHECK_FALSE(declared.local_user->color_rgb.has_value());

    // Firmware echoes the POSTed values back through the RFID path. Since the
    // user declared none of them, every one is firmware truth and must be
    // filed: otherwise these three fields are held by no source at all.
    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PETG"},
                                                    {"MANUFACTURER", "Polymaker"},
                                                    {"ARGB_COLOR", 0xFF00FF00},
                                                    {"CARD_UID", tag_uid},
                                                }})},
                                   });

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->material == "PETG");
    CHECK(lane.vendor_cache->brand == "Polymaker");
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0x00FF00u);
}

// ---------------------------------------------------------------------------
// QIDI Box
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "Qidi's saved ids resolve into a vendor cache",
                 "[lane][ingest][qidi]") {
    QidiHarness harness(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(*harness, filas_list(12, "PETG", 5, "#ED2C2C", 3, "QIDI"));

    QidiBoxTestAccess::parse_vars(*harness, nlohmann::json{{"box_count", 1},
                                                           {"filament_slot0", 12},
                                                           {"color_slot0", 5},
                                                           {"vendor_slot0", 3}});

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->material == "PETG");
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->brand == "QIDI");
    // Saved variables say what a slot was told it holds, never whether it does.
    CHECK_FALSE(lane.vendor_cache->present.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "Qidi files nothing for an id its tables do not resolve",
                 "[lane][ingest][qidi]") {
    QidiHarness harness(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(*harness, filas_list(12, "PETG", 5, "#ED2C2C", 3, "QIDI"));

    // Slot 1's ids name rows the Box's tables do not have. That is not a slot
    // with no filament in it, and it is not a slot holding whatever id 12
    // happens to mean. Slot 0 carries resolvable ids in the same frame, so the
    // loop demonstrably ran and reached the right slot.
    QidiBoxTestAccess::parse_vars(*harness, nlohmann::json{{"box_count", 1},
                                                           {"filament_slot0", 12},
                                                           {"color_slot0", 5},
                                                           {"vendor_slot0", 3},
                                                           {"filament_slot1", 77},
                                                           {"color_slot1", 88},
                                                           {"vendor_slot1", 99}});

    const auto resolved = lane_sources(harness.lane(0));
    REQUIRE(resolved.vendor_cache.has_value());
    CHECK(resolved.vendor_cache->material == "PETG");

    const auto unresolved = lane_sources(harness.lane(1));
    REQUIRE(unresolved.vendor_cache.has_value());
    CHECK_FALSE(unresolved.vendor_cache->material.has_value());
    CHECK_FALSE(unresolved.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(unresolved.vendor_cache->brand.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "Qidi's vendor cache states its tables, not the slot it wrote",
                 "[lane][ingest][qidi]") {
    QidiHarness harness(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(*harness, filas_list(12, "PETG", 5, "#ED2C2C", 3, "QIDI"));

    const nlohmann::json vars{
        {"box_count", 1}, {"filament_slot0", 12}, {"color_slot0", 5}, {"vendor_slot0", 3}};
    QidiBoxTestAccess::parse_vars(*harness, vars);

    // A reload drops every row the ids name. SlotInfo persists across frames
    // and nothing clears it, so the previous frame's values stay on the slot
    // with nothing in the tables behind them.
    QidiBoxTestAccess::apply_filas_list(*harness, filas_list(40, "ABS", 9, "#00FF00", 8, "Elegoo"));
    QidiBoxTestAccess::parse_vars(*harness, vars);

    // Precondition, not the behaviour under test: unless the orphaned values
    // really are still on the slot, a read-back would pass for the wrong reason.
    const auto slot = harness->get_slot_info(0);
    REQUIRE(slot.material == "PETG");
    REQUIRE(slot.color_rgb == 0xED2C2Cu);
    REQUIRE(slot.brand == "QIDI");

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK_FALSE(lane.vendor_cache->material.has_value());
    CHECK_FALSE(lane.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(lane.vendor_cache->brand.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a Qidi palette grey is the no-colour sentinel",
                 "[lane][ingest][qidi]") {
    QidiHarness harness(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(*harness, "[colordict]\n1 = #808080\n2 = #000000\n");

    QidiBoxTestAccess::parse_vars(
        *harness, nlohmann::json{{"box_count", 1}, {"color_slot0", 1}, {"color_slot1", 2}});

    // The palette row reaches the slot, because the Box did name a colour id.
    REQUIRE(harness->get_slot_info(0).color_rgb == helix::AMS_DEFAULT_SLOT_COLOR);

    const auto grey = lane_sources(harness.lane(0));
    REQUIRE(grey.vendor_cache.has_value());
    CHECK_FALSE(grey.vendor_cache->color_rgb.has_value());

    // Black is a colour, and lands one row away in the same table.
    const auto black = lane_sources(harness.lane(1));
    REQUIRE(black.vendor_cache.has_value());
    REQUIRE(black.vendor_cache->color_rgb.has_value());
    CHECK(*black.vendor_cache->color_rgb == 0x000000u);
}

TEST_CASE_METHOD(LVGLTestFixture, "Qidi's presence follows the state word, jam included",
                 "[lane][ingest][qidi]") {
    QidiHarness harness(nullptr, nullptr);

    // One frame, four state words: empty, spooled, seated, and a negative word
    // the Box uses for a faulted lane.
    QidiBoxTestAccess::parse_vars(
        *harness,
        nlohmann::json{{"box_count", 1}, {"slot0", 0}, {"slot1", 1}, {"slot2", 2}, {"slot3", -1}});

    const auto empty = lane_sources(harness.lane(0));
    REQUIRE(empty.sensed.has_value());
    REQUIRE(empty.sensed->present.has_value());
    CHECK(*empty.sensed->present == false);

    const auto spooled = lane_sources(harness.lane(1));
    REQUIRE(spooled.sensed.has_value());
    REQUIRE(spooled.sensed->present.has_value());
    CHECK(*spooled.sensed->present == true);

    const auto seated = lane_sources(harness.lane(2));
    REQUIRE(seated.sensed.has_value());
    REQUIRE(seated.sensed->present.has_value());
    CHECK(*seated.sensed->present == true);

    // A jam is filament stuck in the path. Answering "no reading" here would
    // retract a live presence record at the moment a lane faults.
    REQUIRE(harness->get_slot_info(3).status == helix::SlotStatus::BLOCKED);
    const auto jammed = lane_sources(harness.lane(3));
    REQUIRE(jammed.sensed.has_value());
    REQUIRE(jammed.sensed->present.has_value());
    CHECK(*jammed.sensed->present == true);

    // The state word says nothing about which spool is in the lane.
    CHECK_FALSE(spooled.sensed->material.has_value());
    CHECK_FALSE(spooled.sensed->color_rgb.has_value());
    CHECK_FALSE(spooled.sensed->brand.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a colordict row is read by the tree's hex grammar",
                 "[lane][ingest][qidi]") {
    QidiHarness harness(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(*harness, "[colordict]\n"
                                                  "1 = #ED2C2C\n"
                                                  "2 = F00\n"
                                                  "3 = #11223344\n"
                                                  "4 = 0x00FF00\n"
                                                  "5 = not a colour\n"
                                                  "6 =\n");

    REQUIRE(QidiBoxTestAccess::get_color(*harness, 1).has_value());
    CHECK(*QidiBoxTestAccess::get_color(*harness, 1) == 0xED2C2Cu);
    REQUIRE(QidiBoxTestAccess::get_color(*harness, 2).has_value());
    CHECK(*QidiBoxTestAccess::get_color(*harness, 2) == 0xFF0000u);
    // #RRGGBBAA drops the alpha byte, as it does for every other consumer.
    REQUIRE(QidiBoxTestAccess::get_color(*harness, 3).has_value());
    CHECK(*QidiBoxTestAccess::get_color(*harness, 3) == 0x112233u);
    REQUIRE(QidiBoxTestAccess::get_color(*harness, 4).has_value());
    CHECK(*QidiBoxTestAccess::get_color(*harness, 4) == 0x00FF00u);
    // A row that states no colour is not an id to resolve against.
    CHECK_FALSE(QidiBoxTestAccess::get_color(*harness, 5).has_value());
    CHECK_FALSE(QidiBoxTestAccess::get_color(*harness, 6).has_value());
    CHECK(QidiBoxTestAccess::color_count(*harness) == 4);
}

TEST_CASE_METHOD(LVGLTestFixture, "what the colordict admits decides the write-back id",
                 "[lane][ingest][qidi]") {
    // The stock file's own shape: every row a six-digit #RRGGBB, which is what
    // both records of officiall_filas_list.cfg in this tree contain. Widening
    // the parse admits nothing extra from a file like this.
    const std::string stock = "[colordict]\n1 = #FAFAFA\n2 = #060606\n";

    // Plain backends, not harnesses: this case asks what the palette admits and
    // what resolve_color_id then picks, neither of which needs a lane id, and
    // registering a second harness would clear the first out from under it.
    AmsBackendQidi narrow(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(narrow, stock);
    REQUIRE(QidiBoxTestAccess::color_count(narrow) == 2);

    // A row in one of the newly-admitted spellings. It is a palette MEMBER, so
    // it is also a nearest-match target: resolve_color_id scans the whole map,
    // and set_slot_info writes back the id it picks.
    AmsBackendQidi widened(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(widened, stock + "3 = F11\n");
    REQUIRE(QidiBoxTestAccess::color_count(widened) == 3);
    REQUIRE(QidiBoxTestAccess::get_color(widened, 3).has_value());
    CHECK(*QidiBoxTestAccess::get_color(widened, 3) == 0xFF1111u);

    const auto palette_of = [](const AmsBackendQidi& box, std::initializer_list<int> ids) {
        std::map<int, uint32_t> out;
        for (int id : ids) {
            if (auto rgb = QidiBoxTestAccess::get_color(box, id)) {
                out[id] = *rgb;
            }
        }
        return out;
    };

    // A near-red the stock palette has no good answer for: black is merely the
    // less wrong of two bad matches.
    constexpr uint32_t kNearRed = 0xEE1111u;
    CHECK(QidiBoxTestAccess::resolve_color_id(palette_of(narrow, {1, 2, 3}), kNearRed) == 2);
    CHECK(QidiBoxTestAccess::resolve_color_id(palette_of(widened, {1, 2, 3}), kNearRed) == 3);
}

// ---------------------------------------------------------------------------
// Mock
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "the mock's simulated population becomes lane readings",
                 "[lane][ingest][mock]") {
    MockHarness harness(4);
    REQUIRE(harness->start().success());

    const auto lane = lane_sources(harness.lane(0));

    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);

    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->material == "PLA");
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0x1A1A2Eu);

    // Every field below really is on the slot, so its absence from the record
    // is a decision and not a missing value. The bound is what keeps the mock
    // from being more capable than hardware: no lane's brand or spool binding
    // comes from firmware, and firmware reports a colour as hex, never as a
    // name. Colour NAME is the sharpest of these, because this backend is the
    // only producer in the tree that could file one.
    const auto slot = harness->get_slot_info(0);
    REQUIRE(slot.color_name == "Jet Black");
    REQUIRE(slot.spoolman_id == 1);
    REQUIRE(slot.total_weight_g > 0.0f);
    REQUIRE_FALSE(slot.spool_name.empty());

    CHECK_FALSE(lane.vendor_cache->color_name.has_value());
    CHECK_FALSE(lane.vendor_cache->brand.has_value());
    CHECK_FALSE(lane.vendor_cache->spoolman_id.has_value());
    CHECK_FALSE(lane.vendor_cache->spool_name.has_value());
    CHECK_FALSE(lane.vendor_cache->total_weight_g.has_value());
    CHECK_FALSE(lane.vendor_cache->remaining_weight_g.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "neither arm of the mock's set_slot_info is a reading",
                 "[lane][ingest][mock]") {
    MockHarness harness(4);
    REQUIRE(harness->start().success());

    // persist = true is the editor's path, which reaches the lane model through
    // commit_slot_edit; persist = false is ToolState pushing a Spoolman-linked
    // assignment onto a tool changer's shadow slot. Neither is the machine
    // speaking, so neither disturbs what the population stated.
    auto edited = harness->get_slot_info(0);
    edited.material = "Declared-PC";
    edited.color_rgb = 0x00FF00u;
    edited.brand = "Polymaker";
    REQUIRE(harness->set_slot_info(0, edited, /*persist=*/true).success());

    auto pushed = harness->get_slot_info(1);
    pushed.material = "Pushed-ASA";
    pushed.color_rgb = 0x0000FFu;
    REQUIRE(harness->set_slot_info(1, pushed, /*persist=*/false).success());

    // Preconditions: both writes landed on the slots, so an unchanged record
    // below is a decision and not a call that did nothing.
    REQUIRE(harness->get_slot_info(0).material == "Declared-PC");
    REQUIRE(harness->get_slot_info(0).color_rgb == 0x00FF00u);
    REQUIRE(harness->get_slot_info(1).material == "Pushed-ASA");

    const auto declared = lane_sources(harness.lane(0));
    REQUIRE(declared.vendor_cache.has_value());
    CHECK(declared.vendor_cache->material == "PLA");
    REQUIRE(declared.vendor_cache->color_rgb.has_value());
    CHECK(*declared.vendor_cache->color_rgb == 0x1A1A2Eu);
    CHECK_FALSE(declared.vendor_cache->brand.has_value());

    const auto pushed_lane = lane_sources(harness.lane(1));
    REQUIRE(pushed_lane.vendor_cache.has_value());
    CHECK(pushed_lane.vendor_cache->material == "Silk PLA");
}

TEST_CASE_METHOD(LVGLTestFixture, "the mock's colour sentinel is not a grey anybody chose",
                 "[lane][ingest][mock]") {
    MockHarness harness(4);

    // set_slot_info writes the slot even though it files no reading, which is
    // how a lane gets staged before the population is published.
    auto black = harness->get_slot_info(0);
    black.color_rgb = 0x000000u;
    black.material = "Black-PLA";
    REQUIRE(harness->set_slot_info(0, black, /*persist=*/false).success());

    auto colourless = harness->get_slot_info(1);
    colourless.color_rgb = helix::AMS_DEFAULT_SLOT_COLOR;
    colourless.material = "Grey-PLA";
    REQUIRE(harness->set_slot_info(1, colourless, /*persist=*/false).success());

    REQUIRE(harness->start().success());

    const auto real_black = lane_sources(harness.lane(0));
    REQUIRE(real_black.vendor_cache.has_value());
    REQUIRE(real_black.vendor_cache->color_rgb.has_value());
    CHECK(*real_black.vendor_cache->color_rgb == 0x000000u);

    // Same loop, same publish: the material still lands, so the missing colour
    // is the sentinel rule and not a lane the loop skipped.
    const auto sentinel = lane_sources(harness.lane(1));
    REQUIRE(sentinel.vendor_cache.has_value());
    CHECK(sentinel.vendor_cache->material == "Grey-PLA");
    CHECK_FALSE(sentinel.vendor_cache->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "the factory mock is registered before it starts",
                 "[lane][ingest][mock]") {
    ScopedRuntimeConfig scoped_config;
    get_runtime_config()->test_mode = true;
    get_runtime_config()->use_real_ams = false;
    get_runtime_config()->disable_mock_ams = false;

    auto& ams = helix::AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
    // AmsState::init_subjects observes PrinterState's print-state subject; it
    // must exist first or the observer attaches to nothing.
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);

    REQUIRE(ams.backend_count() == 1);

    // A backend answers INVALID_LANE_ID for every slot until registration
    // stamps its index, so a population published before that lands on no lane
    // and every record here reads empty.
    const auto lane = lane_sources(helix::ams::lane_id_for(0, 0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->material == "PLA");
    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);

    ams.clear_backends();
    helix::ui::UpdateQueue::instance().drain();
    ams.deinit_subjects();
}

// --- The shared namespace, re-read ----------------------------------------
//
// A resync goes back to lane_data and re-reads it. What it FILES is bounded
// twice over.
//
// By source: only records classifying as VendorCache. A record naming a spool
// is the server's statement and one carrying a lock key is a person's, and
// neither becomes true again merely because a re-read saw it.
//
// By lane: only where firmware states no identity of its own. Everywhere else
// a status frame already files the vendor-cache record, ingest() replaces a
// source's record whole, and both arrival orders happen, so a second producer
// there overwrites a fresh reading with a stored one. Nor is a field gained by
// allowing it: the next frame retracts whatever the stored record carried
// beyond what firmware reports. Of the seven backends holding a record store,
// klipper-toolchanger is the only one whose firmware reports no identity, so
// it is the only one that files.

namespace {

/// The Moonraker a record store reads through, with the records a case seeds.
/// A store reaches the database directly, so a harness built with no API of
/// its own still gets a namespace to re-read.
struct LaneDataDb {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ::get_printer_state()};

    /// One lane_data record under @p key.
    void seed(const std::string& key, nlohmann::json record) {
        api.mock_set_db_value("lane_data", key, std::move(record));
    }

    /// A store on the shared namespace, in the key style @p style spells.
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> store(const std::string& backend_id,
                                                                 helix::ams::LaneKeyStyle style) {
        return std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, backend_id, style);
    }
};

/// A tool changer keys its records T<n>; the slot a record names is the inner
/// 0-based field in either style.
std::unique_ptr<helix::ams::FilamentSlotOverrideStore> toolchanger_store(LaneDataDb& db) {
    return db.store("toolchanger", helix::ams::LaneKeyStyle::Tool);
}

} // namespace

// --- The one backend that files -------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "a resync re-reads the shared namespace into the model",
                 "[lane][ingest][resync]") {
    ToolChangerHarness harness(nullptr, nullptr);
    LaneDataDb db;
    db.seed("T0", nlohmann::json{{"lane", "0"}, {"material", "ASA"}, {"color", "#A4B2BC"}});
    ToolChangerTestAccess::inject_override_store(*harness, toolchanger_store(db));

    REQUIRE_FALSE(lane_sources(harness.lane(0)).vendor_cache.has_value());

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->material == "ASA");
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xA4B2BCu);
}

TEST_CASE_METHOD(LVGLTestFixture, "a resync reaches the backend's own block, not slot indices",
                 "[lane][ingest][resync]") {
    ToolChangerHarness harness(nullptr, nullptr);
    LaneDataDb db;
    db.seed("T1", nlohmann::json{{"lane", "1"}, {"material", "PC"}});
    ToolChangerTestAccess::inject_override_store(*harness, toolchanger_store(db));

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->material == "PC");
}

TEST_CASE_METHOD(LVGLTestFixture, "a resync files no declaration for a record naming a spool",
                 "[lane][ingest][resync]") {
    ToolChangerHarness harness(nullptr, nullptr);
    LaneDataDb db;
    db.seed("T0", nlohmann::json{
                      {"lane", "0"}, {"material", "PETG"}, {"color", "#ED2C2C"}, {"spool_id", 42}});
    // A plain record beside it, so the case proves the resync reached the
    // document rather than that it did nothing at all.
    db.seed("T1", nlohmann::json{{"lane", "1"}, {"material", "PLA"}});
    ToolChangerTestAccess::inject_override_store(*harness, toolchanger_store(db));

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    const auto linked = lane_sources(harness.lane(0));
    CHECK_FALSE(linked.spoolman.has_value());
    CHECK_FALSE(linked.vendor_cache.has_value());

    const auto plain = lane_sources(harness.lane(1));
    REQUIRE(plain.vendor_cache.has_value());
    CHECK(plain.vendor_cache->material == "PLA");
}

TEST_CASE_METHOD(LVGLTestFixture, "a resync files no declaration for a locked record",
                 "[lane][ingest][resync]") {
    ToolChangerHarness harness(nullptr, nullptr);
    LaneDataDb db;
    db.seed("T0", nlohmann::json{{"lane", "0"},
                                 {"material", "ABS"},
                                 {"color", "#00FF00"},
                                 {"helix_locked_color", true}});
    db.seed("T1", nlohmann::json{{"lane", "1"}, {"material", "PLA"}});
    ToolChangerTestAccess::inject_override_store(*harness, toolchanger_store(db));

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    // A person's lock is a declaration, and commit_slot_edit is the only
    // funnel that may record one. Filing it here would claim the person
    // declared it again at the moment the screen was opened.
    const auto locked = lane_sources(harness.lane(0));
    CHECK_FALSE(locked.local_user.has_value());
    CHECK_FALSE(locked.vendor_cache.has_value());

    const auto plain = lane_sources(harness.lane(1));
    REQUIRE(plain.vendor_cache.has_value());
    CHECK(plain.vendor_cache->material == "PLA");
}

TEST_CASE_METHOD(LVGLTestFixture, "a re-read that cannot reach the database leaves the lane alone",
                 "[lane][ingest][resync]") {
    ToolChangerHarness harness(nullptr, nullptr);
    LaneDataDb db;
    db.seed("T0", nlohmann::json{{"lane", "0"}, {"material", "PLA"}});
    ToolChangerTestAccess::inject_override_store(*harness, toolchanger_store(db));

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(lane_sources(harness.lane(0)).vendor_cache.has_value());

    db.seed("T0", nlohmann::json{{"lane", "0"}, {"material", "TPU"}});
    db.api.mock_reject_next_db_get();
    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->material == "PLA");
}

// --- The six that do not ---------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture,
                 "a backend whose firmware states identity files nothing from the namespace",
                 "[lane][ingest][resync]") {
    // Each of these files a vendor-cache record from its own status frames, so
    // the persisted record has a live producer to race and nothing to add to
    // it. Only one harness may be live at a time, so each takes its own scope.
    {
        AceHarness harness(nullptr, nullptr);
        LaneDataDb db;
        db.seed("lane1", nlohmann::json{{"lane", "0"}, {"material", "ASA"}});
        AceTestAccess::inject_override_store(*harness,
                                             db.store("ace", helix::ams::LaneKeyStyle::Lane));
        harness->request_resync();
        helix::ui::UpdateQueue::instance().drain();
        INFO("ACE");
        CHECK(helix::ams::known_lanes().empty());
    }
    {
        CfsHarness harness(nullptr, nullptr);
        LaneDataDb db;
        db.seed("lane3", nlohmann::json{{"lane", "2"}, {"material", "PLA-CF"}});
        CfsTestAccess::inject_override_store(*harness,
                                             db.store("cfs", helix::ams::LaneKeyStyle::Lane));
        harness->request_resync();
        helix::ui::UpdateQueue::instance().drain();
        INFO("CFS");
        CHECK(helix::ams::known_lanes().empty());
    }
    {
        SnapmakerHarness harness(nullptr, nullptr);
        LaneDataDb db;
        db.seed("T1", nlohmann::json{{"lane", "1"}, {"material", "ASA"}});
        SnapmakerTestAccess::inject_override_store(
            *harness, db.store("snapmaker", helix::ams::LaneKeyStyle::Tool));
        harness->request_resync();
        helix::ui::UpdateQueue::instance().drain();
        INFO("Snapmaker");
        CHECK(helix::ams::known_lanes().empty());
    }
    {
        Ad5xHarness harness(nullptr, nullptr);
        LaneDataDb db;
        db.seed("lane1", nlohmann::json{{"lane", "0"}, {"material", "PETG"}});
        Ad5xIfsTestAccess::inject_override_store(*harness,
                                                 db.store("ifs", helix::ams::LaneKeyStyle::Lane));
        harness->request_resync();
        helix::ui::UpdateQueue::instance().drain();
        INFO("AD5X IFS");
        CHECK(helix::ams::known_lanes().empty());
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "a backend whose namespace nobody else writes re-reads nothing",
                 "[lane][ingest][resync]") {
    // AFC and Happy Hare keep their overrides in a namespace HelixScreen alone
    // writes, so there is no drift for a re-read to correct. They name no
    // record store at all, which is the other way a backend files nothing.
    {
        AfcHarness harness(nullptr, nullptr);
        harness->request_resync();
        helix::ui::UpdateQueue::instance().drain();
        INFO("AFC");
        CHECK(helix::ams::known_lanes().empty());
    }
    {
        HappyHareHarness harness(nullptr, nullptr);
        harness->request_resync();
        helix::ui::UpdateQueue::instance().drain();
        INFO("Happy Hare");
        CHECK(helix::ams::known_lanes().empty());
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "a resync with no store is a no-op, not a crash",
                 "[lane][ingest][resync]") {
    ToolChangerHarness harness(nullptr, nullptr);

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(helix::ams::known_lanes().empty());
}
