// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The cross-backend census: which Observation fields each producer files, and
// which no producer files at all. Nine backends translate their own firmware
// into one record type, and the agreement between them lives nowhere but here.
// A field added to one backend's translation without a thought for the other
// eight fails a case in this file instead of drifting quietly.
//
// Per-backend BEHAVIOUR - what a sentinel means, what a partial frame
// retracts, what an override must never launder - belongs in
// test_lane_backend_observations.cpp. This file asserts the shape of the
// contract, not the rules inside it.

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
#include "ams_types.h"
#include "lane_observation.h"
#include "lane_source_store.h"
#include "printer_discovery.h"
#include "test_helpers/ace_test_access.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/happy_hare_test_access.h"
#include "test_helpers/qidi_box_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/snapmaker_test_access.h"
#include "test_helpers/toolchanger_test_access.h"
#include "toolchanger_addon.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <tuple>
#include <utility>
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
using helix::ams::LaneId;
using helix::ams::Observation;
using helix::printer::AmsBackendCfs;
using helix::test::RegisteredBackend;

namespace {

/// Every optional field of Observation, in the order Observation::fields()
/// ties them. A field added to that struct has no name here, so the
/// static_assert below stops the build and whoever added it has to say which
/// backends are expected to file it.
constexpr std::array<const char*, 12> kFieldNames = {
    "present",     "color_rgb",          "color_name",         "material",
    "brand",       "spool_name",         "catalog_id",         "product_name",
    "spoolman_id", "spoolman_vendor_id", "remaining_weight_g", "total_weight_g"};

static_assert(std::tuple_size_v<decltype(std::declval<Observation&>().fields())> ==
                  kFieldNames.size(),
              "every Observation field needs a name in kFieldNames");

template <typename Tuple, std::size_t... I>
void name_observed_impl(const Tuple& fields, std::index_sequence<I...>, const std::string& prefix,
                        std::vector<std::string>& out) {
    (
        [&] {
            if (std::get<I>(fields).has_value()) {
                out.push_back(prefix + kFieldNames[I]);
            }
        }(),
        ...);
}

/// "<source>.<field>" for every field @p obs observed. Folds over
/// Observation::fields() so a field added there is censused with no line here.
void name_observed(const Observation& obs, const std::string& prefix,
                   std::vector<std::string>& out) {
    auto fields = obs.fields();
    name_observed_impl(fields, std::make_index_sequence<std::tuple_size_v<decltype(fields)>>{},
                       prefix, out);
}

/// Everything filed on @p lane, across every source, as "<source>.<field>".
/// The list is the lane's whole account of itself: a field missing from it is
/// a field nothing wrote.
std::vector<std::string> filed_on(LaneId lane) {
    const auto sources = lane_sources(lane);
    std::vector<std::string> out;
    if (sources.sensed)
        name_observed(*sources.sensed, "sensed.", out);
    if (sources.spoolman)
        name_observed(*sources.spoolman, "spoolman.", out);
    if (sources.local_user)
        name_observed(*sources.local_user, "local_user.", out);
    if (sources.vendor_cache)
        name_observed(*sources.vendor_cache, "vendor_cache.", out);
    if (sources.metered)
        name_observed(*sources.metered, "metered.", out);
    return out;
}

/// One Moonraker notify_status_update carrying a single Klipper object, which
/// is the envelope five of the nine backends unwrap. Written once rather than
/// per backend: the shape is Moonraker's, not any vendor's.
nlohmann::json status_frame(const std::string& object, nlohmann::json payload) {
    nlohmann::json params;
    params[object] = std::move(payload);
    return nlohmann::json{{"method", "notify_status_update"},
                          {"params", nlohmann::json::array({params, 0.0})}};
}

/// The Box's officiall_filas_list.cfg, carrying the one profile, colour and
/// vendor row the saved ids below name.
constexpr const char* kQidiFilasList = "[fila12]\n"
                                       "filament = PETG Basic\n"
                                       "type = PETG\n"
                                       "min_temp = 230\n"
                                       "max_temp = 250\n"
                                       "[colordict]\n"
                                       "5 = #ED2C2C\n"
                                       "[vendor_list]\n"
                                       "3 = QIDI\n";

/// What one backend's richest real frame put on one lane.
struct BackendCensus {
    std::string backend;
    std::vector<std::string> filed;
};

/// Drive all nine producers, one at a time, each through its own real inbound
/// signal, and report what reached the store.
///
/// Sequential by construction: a RegisteredBackend's constructor calls
/// set_backend(), which clears the backends already registered, so a second
/// live harness would dangle the first. Each block therefore opens and closes
/// before the next begins, and the store it reads is its own.
///
/// Each frame carries every field its backend's translation can set, so a
/// field missing from a census is one that firmware never states rather than
/// one this frame happened to omit. Where a backend has a second parser (AFC's
/// database snapshot, ACE's REST bridge), it fills the same record from the
/// same fields, so one frame censuses both.
std::vector<BackendCensus> census_of_every_backend() {
    std::vector<BackendCensus> out;

    {
        RegisteredBackend<AmsBackendAce> harness(nullptr, nullptr);
        AceTestAccess::handle_status_update(
            *harness,
            status_frame("ace", nlohmann::json{
                                    {"status", "ready"},
                                    {"slots", nlohmann::json::array({nlohmann::json{
                                                  {"status", "ready"},
                                                  {"color", nlohmann::json::array({237, 44, 44})},
                                                  {"type", "PETG"}}})},
                                }));
        out.push_back({"ACE", filed_on(harness.lane(0))});
    }

    {
        RegisteredBackend<AmsBackendAd5xIfs> harness(nullptr, nullptr);
        // The standalone IFS module's own objects. loaded_channels is the
        // decoded silk bitmask (1-based), and ifs_materials is the module's
        // slot table; between them this is everything an AD5X states.
        Ad5xIfsTestAccess::handle_status(
            *harness, nlohmann::json{{"ifs",
                                      {{"connected", true},
                                       {"channel_count", 4},
                                       {"activity", "ready"},
                                       {"active_channel", 1},
                                       {"loaded_channels", nlohmann::json::array({1})}}}});
        Ad5xIfsTestAccess::handle_status(
            *harness,
            nlohmann::json{
                {"ifs_materials",
                 {{"available", true},
                  {"channel_count", 4},
                  {"enabled", true},
                  {"slots", {{"1", {{"type", "PETG"}, {"color", "#ED2C2C"}, {"temp", 250.0}}}}}}}});
        out.push_back({"AD5X IFS", filed_on(harness.lane(0))});
    }

    {
        RegisteredBackend<AmsBackendAfc> harness(nullptr, nullptr);
        AfcTestAccess::initialize_slots(*harness, std::vector<std::string>{"lane1"});
        AfcTestAccess::handle_status_update(
            *harness,
            status_frame("AFC_stepper lane1", nlohmann::json{{"prep", true},
                                                             {"load", true},
                                                             {"tool_loaded", false},
                                                             {"status", "Loaded"},
                                                             {"color", "#ED2C2C"},
                                                             {"material", "PETG"},
                                                             {"filament_name", "Galaxy Black"},
                                                             {"spool_vendor", "Kingroon"},
                                                             {"weight", 612.0},
                                                             {"spool_id", 7},
                                                             {"initial_weight", 1000.0}}));
        out.push_back({"AFC", filed_on(harness.lane(0))});
    }

    {
        RegisteredBackend<AmsBackendCfs> harness(nullptr, nullptr);
        CfsTestAccess::handle_status(
            *harness,
            status_frame("box", nlohmann::json{{"api_version", 1},
                                               {"slots", nlohmann::json::array({nlohmann::json{
                                                             {"index", 0},
                                                             {"material", "PLA"},
                                                             {"brand", "Creality"},
                                                             {"name", "Hyper PLA"},
                                                             {"color", "#ED2C2C"},
                                                             {"present", true},
                                                             {"loaded", false},
                                                             {"spoolman_id", 7}}})}}));
        out.push_back({"CFS", filed_on(harness.lane(0))});
    }

    {
        RegisteredBackend<AmsBackendHappyHare> harness(nullptr, nullptr);
        HappyHareTestAccess::handle_status_update(
            *harness,
            status_frame("mmu",
                         nlohmann::json{{"gate_status", nlohmann::json::array({1})},
                                        {"gate_color", nlohmann::json::array({"ed2c2c"})},
                                        {"gate_material", nlohmann::json::array({"PETG"})},
                                        {"gate_spool_id", nlohmann::json::array({7})},
                                        {"gate_name", nlohmann::json::array({"Galaxy Black"})}}));
        out.push_back({"Happy Hare", filed_on(harness.lane(0))});
    }

    {
        RegisteredBackend<AmsBackendMock> harness(4);
        REQUIRE(harness->start().success());
        out.push_back({"Mock", filed_on(harness.lane(0))});
    }

    {
        RegisteredBackend<AmsBackendQidi> harness(nullptr, nullptr);
        QidiBoxTestAccess::apply_filas_list(*harness, kQidiFilasList);
        // The Box states identity as ids into its own tables and presence as a
        // per-slot state word, both in save_variables. Unwrapped rather than
        // enveloped because this backend's handler reads the object directly.
        QidiBoxTestAccess::handle_status(*harness, nlohmann::json{{"save_variables",
                                                                   {{"variables",
                                                                     {{"box_count", 1},
                                                                      {"slot0", 1},
                                                                      {"filament_slot0", 12},
                                                                      {"color_slot0", 5},
                                                                      {"vendor_slot0", 3}}}}}});
        out.push_back({"Qidi", filed_on(harness.lane(0))});
    }

    {
        RegisteredBackend<AmsBackendSnapmaker> harness(nullptr, nullptr);
        SnapmakerTestAccess::handle_status(
            *harness,
            status_frame(
                "filament_detect",
                nlohmann::json{{"state", nlohmann::json::array({1, 0, 0, 0})},
                               {"info", nlohmann::json::array({nlohmann::json{
                                            {"MAIN_TYPE", "PLA"},
                                            {"SUB_TYPE", "Silk"},
                                            {"MANUFACTURER", "Snapmaker"},
                                            {"ARGB_COLOR", 0xFFED2C2C},
                                            {"WEIGHT", 1000},
                                            {"CARD_UID", nlohmann::json::array({144, 32, 196, 2})},
                                        }})}}));
        out.push_back({"Snapmaker", filed_on(harness.lane(0))});
    }

    {
        RegisteredBackend<AmsBackendToolChanger> harness(nullptr, nullptr);
        harness->set_discovered_tools({"T0", "T1"});
        // AmsState hands a backend its tool list before add_backend() stamps an
        // index, so what initialize_tools() files lands on no lane in
        // production. Drop those records and drive a status frame, which is the
        // path a machine takes.
        helix::ams::reset_lane_sources();
        ToolChangerTestAccess::handle_status(
            *harness,
            status_frame("toolchanger", nlohmann::json{{"status", "ready"}, {"tool_number", 0}}));
        out.push_back({"ToolChanger", filed_on(harness.lane(0))});
    }

    return out;
}

/// The census as the contract states it. Ordered as Observation ties its
/// fields, which is the order filed_on() reports them in.
///
/// Changing a line here is changing what a backend tells the rest of the tree,
/// so it is the place to weigh whether the other eight should follow.
const std::vector<BackendCensus>& expected_census() {
    static const std::vector<BackendCensus> table = {
        // The hub reports an occupancy status and its own memory of a bay's
        // colour and type. It reads no tag and weighs nothing.
        {"ACE", {"sensed.present", "vendor_cache.color_rgb", "vendor_cache.material"}},
        // Adventurer5M.json remembers colour and type across an eject; the silk
        // sensors answer presence.
        {"AD5X IFS", {"sensed.present", "vendor_cache.color_rgb", "vendor_cache.material"}},
        // The widest producer in the fleet, and the only one that meters: AFC's
        // own store keeps four identity fields and a Spoolman binding, and its
        // weight is a second source rather than more of the same record.
        {"AFC",
         {"sensed.present", "vendor_cache.color_rgb", "vendor_cache.material", "vendor_cache.brand",
          "vendor_cache.spool_name", "vendor_cache.spoolman_id", "metered.remaining_weight_g",
          "metered.total_weight_g"}},
        // The CFS reads a tag that names a PRODUCT, which is a different
        // question from what a spool is called.
        {"CFS",
         {"sensed.present", "vendor_cache.color_rgb", "vendor_cache.material", "vendor_cache.brand",
          "vendor_cache.product_name", "vendor_cache.spoolman_id"}},
        // mmu_vars.cfg remembers a declaration somebody made once. Gate status
        // is the only thing Happy Hare senses.
        {"Happy Hare",
         {"sensed.present", "vendor_cache.color_rgb", "vendor_cache.material",
          "vendor_cache.spool_name", "vendor_cache.spoolman_id"}},
        // Bounded to what real firmware states: the simulated population carries
        // a brand, a spool name, a colour name, a Spoolman id and a weight, and
        // files none of them, because no machine in the fleet reports them.
        //
        // It is not a producer on the same footing as the eight above. Its one
        // SlotInfo is both the simulated machine's state and the user's edits,
        // with no firmware store behind it, so it can only state readings
        // where no edit has landed yet: it files once, at the tail of start(),
        // and its presence reading does not follow the simulated load. The
        // single call site is what holds that, and a lint gate in
        // tests/shell/test_code_lint.bats holds the single call site.
        {"Mock", {"sensed.present", "vendor_cache.color_rgb", "vendor_cache.material"}},
        // Saved ids resolved against the Box's own tables. A jam still reports
        // filament, so presence comes from the state word.
        {"Qidi",
         {"sensed.present", "vendor_cache.color_rgb", "vendor_cache.material",
          "vendor_cache.brand"}},
        // The one firmware in the fleet whose tag states a weight, which makes
        // this the only total_weight_g filed as identity rather than as a meter
        // reading. AFC's total is the meter's.
        {"Snapmaker",
         {"sensed.present", "vendor_cache.color_rgb", "vendor_cache.material", "vendor_cache.brand",
          "vendor_cache.product_name", "vendor_cache.total_weight_g"}},
        // klipper-toolchanger senses docking and states nothing else. A lane
        // here carries no identity record at all.
        {"ToolChanger", {"sensed.present"}},
    };
    return table;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "every backend files exactly the fields the census names",
                 "[lane][ingest][census]") {
    const auto measured = census_of_every_backend();
    const auto& expected = expected_census();

    REQUIRE(measured.size() == expected.size());

    for (size_t i = 0; i < measured.size(); ++i) {
        INFO("backend: " << measured[i].backend);
        REQUIRE(measured[i].backend == expected[i].backend);
        CHECK(measured[i].filed == expected[i].filed);
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "no backend frame translation files a catalog id, a vendor id or a colour name",
                 "[lane][ingest][census]") {
    // Three fields Observation carries that no producer writes. A consumer or
    // a test resting on one of them is resting on a value nothing supplies.
    //
    // Scoped to frame translation on purpose: catalog_id and
    // spoolman_vendor_id DO reach a lane through the resync path, which reads
    // the shared override namespace and files a declaration rather than a
    // reading. Nothing a machine says carries either.
    const std::vector<std::string> dead = {"catalog_id", "spoolman_vendor_id", "color_name"};

    for (const auto& entry : census_of_every_backend()) {
        INFO("backend: " << entry.backend);
        // The positive control, on this backend's own frame: unless something
        // reached the store, an absence below would only mean the drive did
        // nothing.
        REQUIRE_FALSE(entry.filed.empty());

        for (const auto& filed : entry.filed) {
            // Every entry is "<source>.<field>", and the claim is about the
            // field whichever source filed it.
            const std::string field = filed.substr(filed.find('.') + 1);
            INFO("filed: " << filed);
            CHECK(std::find(dead.begin(), dead.end(), field) == dead.end());
        }
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "a firmware name reaches the field its own vendor means by it",
                 "[lane][ingest][census]") {
    // Two vendors publish a per-lane name and the model routes them to
    // different fields. Both are right, and the divergence is the reason the
    // pair is pinned together rather than in either backend's own file.
    {
        // Happy Hare's gate_name is what somebody called the SPOOL in that
        // gate.
        RegisteredBackend<AmsBackendHappyHare> harness(nullptr, nullptr);
        HappyHareTestAccess::handle_status_update(
            *harness,
            status_frame("mmu",
                         nlohmann::json{{"gate_status", nlohmann::json::array({1})},
                                        {"gate_name", nlohmann::json::array({"Galaxy Black"})}}));

        const auto lane = lane_sources(harness.lane(0));
        REQUIRE(lane.vendor_cache.has_value());
        CHECK(lane.vendor_cache->spool_name == "Galaxy Black");
        CHECK_FALSE(lane.vendor_cache->product_name.has_value());
    }

    {
        // The CFS tag's `name` is which PRODUCT the bay holds.
        RegisteredBackend<AmsBackendCfs> harness(nullptr, nullptr);
        CfsTestAccess::handle_status(
            *harness,
            status_frame("box", nlohmann::json{{"api_version", 1},
                                               {"slots", nlohmann::json::array({nlohmann::json{
                                                             {"index", 0},
                                                             {"material", "PLA"},
                                                             {"name", "Hyper PLA"},
                                                             {"present", true},
                                                             {"loaded", false}}})}}));

        const auto lane = lane_sources(harness.lane(0));
        REQUIRE(lane.vendor_cache.has_value());
        CHECK(lane.vendor_cache->product_name == "Hyper PLA");
        CHECK_FALSE(lane.vendor_cache->spool_name.has_value());
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "Snapmaker's observation and its SlotInfo part over SUB_TYPE",
                 "[lane][ingest][census]") {
    RegisteredBackend<AmsBackendSnapmaker> harness(nullptr, nullptr);

    SnapmakerTestAccess::handle_status(
        *harness,
        status_frame(
            "filament_detect",
            nlohmann::json{{"state", nlohmann::json::array({1, 0, 0, 0})},
                           {"info", nlohmann::json::array({nlohmann::json{
                                        {"MAIN_TYPE", "PLA"},
                                        {"SUB_TYPE", "Silk"},
                                        {"MANUFACTURER", "Snapmaker"},
                                        {"ARGB_COLOR", 0xFFED2C2C},
                                        {"CARD_UID", nlohmann::json::array({144, 32, 196, 2})},
                                    }})}}));

    // One parse, two destinations, and they deliberately disagree: the record
    // calls SUB_TYPE the product line it is, while SlotInfo keeps the spelling
    // the UI has always read. Reading one and expecting the other is the
    // mistake this case exists to catch.
    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->product_name == "Silk");
    CHECK_FALSE(lane.vendor_cache->spool_name.has_value());
    CHECK(harness->get_slot_info(0).spool_name == "Silk");
}

TEST_CASE_METHOD(LVGLTestFixture, "a tool changer's presence reading is a dock, not a filament",
                 "[lane][ingest][census]") {
    RegisteredBackend<AmsBackendToolChanger> harness(nullptr, nullptr);
    harness->set_discovered_tools({"T0", "T1", "T2"});

    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "tool T2",
                                            "pin_watch io", "servo my_servo", "extruder"}));
    harness->set_tool_sensor(helix::toolchanger_addon::resolve_tool_sensor(hw));

    helix::ams::reset_lane_sources();

    // T1 is on the carriage and T2's dock reads vacant, which means that hot
    // end has been taken out of the machine. Nothing in this frame, or in
    // anything klipper-toolchanger publishes, says whether filament is in any
    // of them.
    ToolChangerTestAccess::handle_status(
        *harness,
        status_frame("medusahc",
                     nlohmann::json{{"operation", "idle"},
                                    {"current_tool", 1},
                                    {"sensors", {{"e", 1}, {"t0", 1}, {"t1", 0}, {"t2", 0}}}}));

    const auto docked = lane_sources(harness.lane(0));
    REQUIRE(docked.sensed.has_value());
    REQUIRE(docked.sensed->present.has_value());
    CHECK(*docked.sensed->present == true);

    const auto carriage = lane_sources(harness.lane(1));
    REQUIRE(carriage.sensed.has_value());
    REQUIRE(carriage.sensed->present.has_value());
    CHECK(*carriage.sensed->present == true);

    // The discriminator: presence tracks the dock. A toolhead removed from the
    // machine is the only thing that makes this false, and a spool leaving a
    // toolhead can never make it false.
    const auto removed = lane_sources(harness.lane(2));
    REQUIRE(removed.sensed.has_value());
    REQUIRE(removed.sensed->present.has_value());
    CHECK(*removed.sensed->present == false);

    // A consumer treating present as "filament is loaded" has nothing here to
    // tell it otherwise: no identity record exists on any of the three lanes.
    CHECK_FALSE(docked.vendor_cache.has_value());
    CHECK_FALSE(carriage.vendor_cache.has_value());
    CHECK_FALSE(removed.vendor_cache.has_value());
}
