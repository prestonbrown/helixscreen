// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ingest_legacy_records() is what puts a user's pre-source-model lane_data
// records into the lane source store at backend init, before anything reads
// a lane. These cases exercise it directly against a FilamentSlotOverrideStore
// loaded from a mock Moonraker DB, the same shape every backend's on_started()
// hands it.

#include "ams_types.h"
#include "filament_slot_override_store.h"
#include "helix_test_fixture.h"
#include "lane_apply.h"
#include "lane_legacy_migration.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ams::FilamentSlotOverrideStore;
using helix::ams::file_lane_sources;
using helix::ams::ingest;
using helix::ams::ingest_legacy_records;
using helix::ams::lane_id_for;
using helix::ams::lane_sources;
using helix::ams::LegacyLockKeys;
using helix::ams::Observation;
using helix::ams::ObservationSource;
using helix::ams::resolved_lane;

TEST_CASE_METHOD(HelixTestFixture, "Loading a namespace populates each lane's sources",
                 "[lane][migration]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    api.mock_set_db_value(
        "lane_data", "lane1",
        nlohmann::json{
            {"lane", 0}, {"spool_id", 7}, {"color", "#FFFFFF"}, {"helix_locked_color", true}});
    api.mock_set_db_value(
        "lane_data", "lane2",
        nlohmann::json{{"lane", 1}, {"color", "#ED2C2C"}, {"helix_material", "PLA"}});
    api.mock_set_db_value("lane_data", "seated", nlohmann::json(0));

    FilamentSlotOverrideStore store(&api, "ad5x_ifs");
    const auto loaded = store.load_blocking();
    REQUIRE(loaded.size() == 2);

    const int populated =
        ingest_legacy_records(store, LegacyLockKeys::LaneData, /*backend_index=*/0);
    CHECK(populated == 2);

    // Lane 0 was linked, so its white landed on the server's rung despite the
    // lock flag. color_rgb is the deciding field and spoolman is the deciding
    // source.
    const auto lane0 = lane_sources(lane_id_for(0, 0));
    REQUIRE(lane0.spoolman.has_value());
    CHECK(lane0.spoolman->color_rgb == 0xFFFFFFu);
    CHECK_FALSE(lane0.local_user.has_value());

    // Lane 1 carried no lock key, so it is a cache.
    const auto lane1 = lane_sources(lane_id_for(0, 1));
    REQUIRE(lane1.remembered.has_value());
    CHECK(lane1.remembered->color_rgb == 0xED2C2Cu);
    CHECK_FALSE(lane1.local_user.has_value());

    // Nothing stored is a presence signal, so neither lane carries a presence
    // reading at all. Migration must not invent one in either direction.
    CHECK_FALSE(resolved_lane(lane_id_for(0, 0)).present.has_value());
    CHECK_FALSE(resolved_lane(lane_id_for(0, 1)).present.has_value());
}

TEST_CASE_METHOD(HelixTestFixture,
                 "A locked, unlinked record's colour reaches the store as the user's own",
                 "[lane][migration]") {
    // The defect this task exists to prevent: ingest() silently refuses a
    // LocalUser observation, so a record with a lock key set must be routed
    // through commit_slot_edit() or the user's declaration never reaches the
    // store at all.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    api.mock_set_db_value(
        "lane_data", "lane1",
        nlohmann::json{{"lane", 0}, {"color", "#3355FF"}, {"helix_locked_color", true}});

    FilamentSlotOverrideStore store(&api, "ad5x_ifs");
    store.load_blocking();

    const int populated =
        ingest_legacy_records(store, LegacyLockKeys::LaneData, /*backend_index=*/0);
    CHECK(populated == 1);

    const auto lane = lane_sources(lane_id_for(0, 0));
    REQUIRE(lane.local_user.has_value());
    CHECK(lane.local_user->color_rgb == 0x3355FFu);
    CHECK_FALSE(lane.remembered.has_value());
}

TEST_CASE_METHOD(HelixTestFixture, "Ingesting the same namespace twice changes nothing",
                 "[lane][migration]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    api.mock_set_db_value("lane_data", "lane1",
                          nlohmann::json{{"lane", 0},
                                         {"color", "#BCBCBC"},
                                         {"helix_material", "PLA"},
                                         {"helix_locked_color", true}});

    FilamentSlotOverrideStore store(&api, "ad5x_ifs");
    store.load_blocking();
    ingest_legacy_records(store, LegacyLockKeys::LaneData, 0);
    const auto first = resolved_lane(lane_id_for(0, 0));

    store.load_blocking();
    ingest_legacy_records(store, LegacyLockKeys::LaneData, 0);
    const auto second = resolved_lane(lane_id_for(0, 0));

    CHECK(second.color_rgb == first.color_rgb);
    CHECK(second.material == first.material);
    CHECK(second.present == first.present);
    CHECK(second.spoolman_id == first.spoolman_id);
}

TEST_CASE_METHOD(HelixTestFixture, "Classification reads the document the store actually received",
                 "[lane][migration]") {
    // A record with a colour and NO lock key is not this application's word:
    // a missing key is never the user's declaration, whatever value it sits
    // beside. With no helix_ key of any kind on the document, what wrote it
    // is another tool (#1632), and its colour is that tool's statement.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    api.mock_set_db_value("lane_data", "lane1", nlohmann::json{{"lane", 0}, {"color", "#ED2C2C"}});

    FilamentSlotOverrideStore store(&api, "ad5x_ifs");
    const auto loaded = store.load_blocking();

    // The load rule has already refused the missing key.
    REQUIRE_FALSE(helix::ams::declares_color(loaded.at(0)));

    ingest_legacy_records(store, LegacyLockKeys::LaneData, 0);
    const auto lane = lane_sources(lane_id_for(0, 0));
    REQUIRE(lane.local_user.has_value());
    CHECK(lane.local_user->color_rgb == 0xED2C2Cu);
    CHECK_FALSE(lane.remembered.has_value());
}

TEST_CASE_METHOD(HelixTestFixture, "A load that falls back to the on-disk cache ingests nothing",
                 "[lane][migration]") {
    // The cache-fallback path (load_blocking's offline branch) never populates
    // last_lane_data_records(): there is no wire document to classify a cached
    // record against, so ingesting it would be a guess rather than a reading.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    api.mock_reject_next_db_get();

    FilamentSlotOverrideStore store(&api, "ad5x_ifs");
    store.load_blocking();

    CHECK(store.last_lane_data_records().empty());
    const int populated = ingest_legacy_records(store, LegacyLockKeys::LaneData, 0);
    CHECK(populated == 0);
}

// ============================================================================
// Per-field authorship. Every identity field answers from the declared set.
// Colour and material also carry lock keys on the wire, written from the set,
// which is how a record written before the set could name them said who chose
// them.
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture,
                 "A user's typed brand outlives a firmware frame that states another",
                 "[lane][migration]") {
    // brand is editable in the spool editor (AmsEditOverlay::is_dirty), so a
    // stored brand can be the user's own word. Filing it as merely remembered
    // would put it below the machine, and the next frame carrying any brand
    // would take the lane back.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ad5x_ifs");

    // The edit is a brand and nothing else: every other SlotInfo field rests
    // on its "nothing here" default, so the record declares exactly one field.
    const helix::SlotInfo empty_lane;
    helix::SlotInfo edited;
    edited.brand = "Hatchbox";
    bool saved = false;
    store.save_async(0, helix::ams::user_override_from_slot_info(empty_lane, edited, nullptr),
                     [&](bool, std::string) { saved = true; });
    REQUIRE(saved);

    REQUIRE(store.load_blocking().size() == 1);
    REQUIRE(ingest_legacy_records(store, LegacyLockKeys::LaneData, /*backend_index=*/0) == 1);

    const helix::ams::LaneId lane = lane_id_for(0, 0);
    const auto sources = lane_sources(lane);
    REQUIRE(sources.local_user.has_value());
    REQUIRE(sources.local_user->brand.has_value());
    CHECK(*sources.local_user->brand == "Hatchbox");

    // The machine now states a brand of its own. LocalUser outranks
    // VendorCache, so the user's word stands.
    Observation frame(ObservationSource::VendorCache);
    frame.brand = "Generic";
    ingest(lane, frame);
    CHECK(resolved_lane(lane).brand == "Hatchbox");
}

TEST_CASE_METHOD(HelixTestFixture, "A brand nobody declared yields to the next firmware frame",
                 "[lane][migration]") {
    // A record carries brands no person typed: a co-author in the shared
    // namespace writes vendor_name, and our own emit mirrors what firmware
    // said. Declaring none of them keeps the weakest rung, where a machine
    // stating the field corrects it.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    api.mock_set_db_value("lane_data", "lane1",
                          nlohmann::json{{"lane", 0},
                                         {"vendor_name", "Firmware Brand"},
                                         {"helix_locked_color", false},
                                         {"helix_locked_material", false},
                                         {"helix_declared", nlohmann::json::array()}});

    FilamentSlotOverrideStore store(&api, "ad5x_ifs");
    REQUIRE(store.load_blocking().size() == 1);
    REQUIRE(ingest_legacy_records(store, LegacyLockKeys::LaneData, 0) == 1);

    const helix::ams::LaneId lane = lane_id_for(0, 0);
    const auto sources = lane_sources(lane);
    CHECK_FALSE(sources.local_user.has_value());
    REQUIRE(sources.remembered.has_value());
    REQUIRE(sources.remembered->brand.has_value());
    CHECK(*sources.remembered->brand == "Firmware Brand");

    Observation frame(ObservationSource::VendorCache);
    frame.brand = "Corrected Brand";
    ingest(lane, frame);
    CHECK(resolved_lane(lane).brand == "Corrected Brand");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "A legacy record's brand is the user's word only beside a true lock",
                 "[lane][migration]") {
    // A record with no helix_declared key was written by an older build, and
    // its brand counts as declared only beside a colour or material
    // declaration on the same record, which a true lock key over a value is.
    // That declaration is the evidence a person edited the record: the
    // auto-mirror declares nothing and can populate no brand of its own. The
    // helix_material key is what keeps the document ours: without a helix_
    // key of any kind it would be another tool's write outright (#1632).
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    nlohmann::json record{
        {"lane", 0}, {"vendor", "Hatchbox"}, {"color", "#3355FF"}, {"helix_material", "PLA"}};
    const bool locked = GENERATE(true, false);
    if (locked) {
        record["helix_locked_color"] = true;
    }
    api.mock_set_db_value("lane_data", "lane1", record);

    FilamentSlotOverrideStore store(&api, "ad5x_ifs");
    REQUIRE(store.load_blocking().size() == 1);
    REQUIRE(ingest_legacy_records(store, LegacyLockKeys::LaneData, 0) == 1);

    const helix::ams::LaneId lane = lane_id_for(0, 0);
    const auto sources = lane_sources(lane);
    if (locked) {
        REQUIRE(sources.local_user.has_value());
        REQUIRE(sources.local_user->brand.has_value());
        CHECK(*sources.local_user->brand == "Hatchbox");
    } else {
        REQUIRE(sources.remembered.has_value());
        REQUIRE(sources.remembered->brand.has_value());
        CHECK(*sources.remembered->brand == "Hatchbox");
        CHECK_FALSE(sources.local_user.has_value());
    }

    // Either way the ranking decides the outcome, so state it at the lane.
    Observation frame(ObservationSource::VendorCache);
    frame.brand = "Firmware Brand";
    ingest(lane, frame);
    CHECK(resolved_lane(lane).brand == (locked ? "Hatchbox" : "Firmware Brand"));
}

TEST_CASE_METHOD(HelixTestFixture,
                 "A legacy record's true lock does not license an empty brand it never carried",
                 "[lane][migration]") {
    // The trap: "some identity field was ever locked" is not evidence THIS
    // field was ever declared. A legacy record with a locked material and no
    // brand at all must not have that lock stand in for a user's clear, or no
    // later firmware brand could ever land.
    using helix::ams::from_lane_data_record;
    using helix::ams::sources_from_record;

    const nlohmann::json wire{{"lane", 0}, {"helix_locked_material", true}};
    const auto parsed = from_lane_data_record(wire);
    REQUIRE(parsed.has_value());

    const auto sources = sources_from_record(parsed->second, wire, LegacyLockKeys::LaneData);
    CHECK_FALSE(sources.local_user.has_value());
    CHECK_FALSE(sources.remembered.has_value());

    const helix::ams::LaneId lane = lane_id_for(0, 0);
    CHECK_FALSE(file_lane_sources(lane, sources));

    Observation frame(ObservationSource::VendorCache);
    frame.brand = "Firmware Brand";
    ingest(lane, frame);
    CHECK(resolved_lane(lane).brand == "Firmware Brand");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "A field the declared set never named stays skipped when it is empty",
                 "[lane][migration]") {
    // The key being present at all must not turn every empty field into a
    // declaration - only the field the set actually names may be filed that
    // way. An empty declared set on a modern record means the same thing a
    // keyless legacy record with no lock means: the user never touched it.
    using helix::ams::from_lane_data_record;
    using helix::ams::sources_from_record;

    const nlohmann::json wire{{"lane", 0},
                              {"helix_locked_color", false},
                              {"helix_locked_material", false},
                              {"helix_declared", nlohmann::json::array()}};
    const auto parsed = from_lane_data_record(wire);
    REQUIRE(parsed.has_value());

    const auto sources = sources_from_record(parsed->second, wire, LegacyLockKeys::LaneData);
    CHECK_FALSE(sources.local_user.has_value());
    CHECK_FALSE(sources.remembered.has_value());

    const helix::ams::LaneId lane = lane_id_for(0, 0);
    CHECK_FALSE(file_lane_sources(lane, sources));

    Observation frame(ObservationSource::VendorCache);
    frame.brand = "Firmware Brand";
    ingest(lane, frame);
    CHECK(resolved_lane(lane).brand == "Firmware Brand");
}

TEST_CASE_METHOD(HelixTestFixture, "A record another tool wrote files as the lane's statement",
                 "[lane][migration]") {
    // Mainsail's spool dialog and Orca's printer agent write lane_data with
    // none of our authorship keys, replacing whatever record stood there. The
    // newest edit wins whoever made it (prestonbrown/helixscreen#1632), so a
    // foreign record's identity is a statement about the lane, not a memory:
    // it files on the user's rung and outranks what firmware's cache says.
    using helix::ams::from_lane_data_record;
    using helix::ams::sources_from_record;

    const nlohmann::json wire{{"lane", 0},
                              {"color", "#ED2C2C"},
                              {"material", "PLA"},
                              {"bed_temp", 60},
                              {"nozzle_temp", 200}};
    const auto parsed = from_lane_data_record(wire);
    REQUIRE(parsed.has_value());

    const auto sources = sources_from_record(parsed->second, wire, LegacyLockKeys::LaneData);
    REQUIRE(sources.local_user.has_value());
    REQUIRE(sources.local_user->color_rgb.has_value());
    CHECK(*sources.local_user->color_rgb == 0xED2C2Cu);
    CHECK(sources.local_user->material == "PLA");
    CHECK_FALSE(sources.remembered.has_value());
    // Temps are not lane-model identity, and no weight rode in with this
    // record, so nothing files as metered.
    CHECK_FALSE(sources.metered.has_value());

    const helix::ams::LaneId lane = lane_id_for(0, 0);
    file_lane_sources(lane, sources);
    Observation frame(ObservationSource::VendorCache);
    frame.color_rgb = 0x00AEFFu;
    frame.material = "PETG";
    ingest(lane, frame);
    const auto resolved = resolved_lane(lane);
    REQUIRE(resolved.color_rgb.has_value());
    CHECK(*resolved.color_rgb == 0xED2C2Cu);
    CHECK(resolved.material == "PLA");
}

TEST_CASE_METHOD(HelixTestFixture, "A record's scan_time becomes its statement's stamp",
                 "[lane][migration]") {
    using helix::ams::from_lane_data_record;
    using helix::ams::sources_from_record;

    const nlohmann::json wire{{"lane", 0},
                              {"color", "#ED2C2C"},
                              {"material", "PLA"},
                              {"scan_time", "2026-09-25T12:00:00Z"}};
    const auto parsed = from_lane_data_record(wire);
    REQUIRE(parsed.has_value());

    const auto sources = sources_from_record(parsed->second, wire, LegacyLockKeys::LaneData);
    REQUIRE(sources.local_user.has_value());
    REQUIRE(sources.local_user->edited_at.has_value());
    CHECK(*sources.local_user->edited_at == parsed->second.updated_at);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "Our own record coming back files as remembered, not a statement",
                 "[lane][migration]") {
    // The authorship keys are what tell our own write from another tool's.
    // A record carrying them files by its declared bits however fresh its
    // scan_time is, so re-reading our own record never promotes it over the
    // edit that wrote it.
    using helix::ams::from_lane_data_record;
    using helix::ams::sources_from_record;

    const nlohmann::json wire{{"lane", 0},
                              {"color", "#ED2C2C"},
                              {"material", "PLA"},
                              {"helix_declared", nlohmann::json::array()},
                              {"helix_locked_color", false},
                              {"helix_locked_material", false},
                              {"scan_time", "2026-09-25T13:00:00Z"}};
    const auto parsed = from_lane_data_record(wire);
    REQUIRE(parsed.has_value());

    const auto sources = sources_from_record(parsed->second, wire, LegacyLockKeys::LaneData);
    REQUIRE(sources.remembered.has_value());
    CHECK_FALSE(sources.local_user.has_value());
}

TEST_CASE_METHOD(HelixTestFixture,
                 "A record our legacy mirror wrote is not another tool's statement",
                 "[lane][migration]") {
    // The 0.99.x auto-mirror wrote lane_data with `vendor` and `spool_name`
    // and none of today's helix_ keys, so the authorship question has to
    // answer those spellings as ours: reading such a mirror as foreign would
    // promote it to the user's rung at load and paint over the live tag
    // reading. The namespace's shared spellings (`vendor_name`, `name`) stay
    // another tool's, whatever values they carry.
    using helix::ams::from_lane_data_record;
    using helix::ams::sources_from_record;

    const nlohmann::json mirror{{"lane", 0},
                                {"color", "#ED2C2C"},
                                {"material", "PLA"},
                                {"vendor", "AFC Basics"},
                                {"spool_name", "Quiet PLA"},
                                {"scan_time", "2026-09-25T12:00:00Z"}};
    const auto parsed = from_lane_data_record(mirror);
    REQUIRE(parsed.has_value());

    const auto ours = sources_from_record(parsed->second, mirror, LegacyLockKeys::LaneData);
    CHECK_FALSE(ours.local_user.has_value());
    REQUIRE(ours.remembered.has_value());
    CHECK(ours.remembered->brand == "AFC Basics");
    CHECK(ours.remembered->spool_name == "Quiet PLA");

    // The same identity under the shared spellings: a foreign document.
    const nlohmann::json foreign{{"lane", 0},           {"color", "#ED2C2C"},
                                 {"material", "PLA"},   {"vendor_name", "AFC Basics"},
                                 {"name", "Quiet PLA"}, {"scan_time", "2026-09-25T12:00:00Z"}};
    const auto parsed_foreign = from_lane_data_record(foreign);
    REQUIRE(parsed_foreign.has_value());

    const auto theirs =
        sources_from_record(parsed_foreign->second, foreign, LegacyLockKeys::LaneData);
    REQUIRE(theirs.local_user.has_value());
    CHECK_FALSE(theirs.remembered.has_value());
}

TEST_CASE_METHOD(HelixTestFixture,
                 "A record AFC's own plugin wrote is firmware, not an outside edit",
                 "[lane][migration]") {
    // send_lane_data writes the lane's record with its own bookkeeping keys
    // (td, lane, extruder_index) and scan_time = the TD-1's scan time, ""
    // without a TD-1. Both spellings must file as readings, never as the
    // lane's statement: the empty one would win the promotion outright, and
    // the scan-time one would win it whenever a rescan landed after the
    // user's edit, even though a scan clock is not an edit clock.
    using helix::ams::from_lane_data_record;
    using helix::ams::sources_from_record;

    const auto plugin_record = [](const char* scan_time) {
        return nlohmann::json{{"lane", 0},           {"td", "1"},
                              {"extruder_index", 0}, {"color", "#ED2C2C"},
                              {"material", "PLA"},   {"bed_temp", 60},
                              {"nozzle_temp", 210},  {"spool_id", nullptr},
                              {"weight", 1000},      {"scan_time", scan_time}};
    };

    for (const char* scan_time : {"", "2026-09-25T13:00:00Z"}) {
        const nlohmann::json wire = plugin_record(scan_time);
        const auto parsed = from_lane_data_record(wire);
        REQUIRE(parsed.has_value());

        const auto sources = sources_from_record(parsed->second, wire, LegacyLockKeys::LaneData);
        CHECK_FALSE(sources.local_user.has_value());
        REQUIRE(sources.remembered.has_value());
        CHECK(sources.remembered->color_rgb == 0xED2C2Cu);
        CHECK(sources.remembered->material == "PLA");
    }
}

TEST_CASE("An outside record displaces only a statement it is newer than", "[lane][migration]") {
    using helix::ams::outside_edit_wins;

    helix::ams::FilamentSlotOverride record;
    helix::ams::Observation standing(ObservationSource::LocalUser);

    // Nothing standing: the record is the only statement anyone made.
    CHECK(outside_edit_wins(record, std::nullopt));
    CHECK(outside_edit_wins(record, standing));

    const auto at = [](int hours) {
        return std::chrono::system_clock::time_point{
            std::chrono::seconds(1790337600 + hours * 3600)};
    };
    record.updated_at = at(1);

    // Stamped by its writer: newer displaces, equal and older stay below the
    // lane's own edit.
    standing.edited_at = at(0);
    CHECK(outside_edit_wins(record, standing));
    standing.edited_at = at(1);
    CHECK_FALSE(outside_edit_wins(record, standing));
    standing.edited_at = at(2);
    CHECK_FALSE(outside_edit_wins(record, standing));

    // Unstampable: a foreign writer that writes no scan_time replaced our
    // stamped record wholesale, so theirs is the newest edit there is.
    record.updated_at = {};
    CHECK(outside_edit_wins(record, standing));
}

TEST_CASE("A foreign stamp in JS or Python spelling still orders", "[lane][migration]") {
    // toISOString() writes fractional seconds, isoformat() writes a numeric
    // offset: both name an instant, and reading either as unstamped would let
    // a stale foreign record win outright over a newer user statement.
    using helix::ams::from_lane_data_record;
    using helix::ams::outside_edit_wins;

    const auto stamp = [](const char* scan_time) {
        const nlohmann::json wire{{"lane", 0}, {"color", "#ED2C2C"}, {"scan_time", scan_time}};
        const auto parsed = from_lane_data_record(wire);
        REQUIRE(parsed.has_value());
        return parsed->second.updated_at;
    };

    // The fraction is sub-second, not a reject: .500Z sits strictly between
    // the plain second and the next one.
    const auto half_past = stamp("2026-09-25T12:00:00.500Z");
    CHECK(half_past > stamp("2026-09-25T12:00:00Z"));
    CHECK(half_past < stamp("2026-09-25T12:00:01Z"));

    // An offset is a zone, not garbage: 14:00 at +02:00 and 10:00 at -02:00
    // are both 12:00 UTC.
    CHECK(stamp("2026-09-25T14:00:00+02:00") == stamp("2026-09-25T12:00:00Z"));
    CHECK(stamp("2026-09-25T10:00:00-02:00") == stamp("2026-09-25T12:00:00Z"));

    // A zoneless wall time names no instant and reads as unstamped.
    CHECK(stamp("2026-09-25T12:00:00").time_since_epoch().count() == 0);

    // The pay-off: a fractional stamp older than the statement does not win,
    // where an unparsable one read as "no stamp" and won outright.
    helix::ams::Observation standing(ObservationSource::LocalUser);
    standing.edited_at = stamp("2026-09-25T13:00:00Z");
    helix::ams::FilamentSlotOverride record;
    record.updated_at = stamp("2026-09-25T12:00:00.123Z");
    CHECK_FALSE(outside_edit_wins(record, standing));
}

TEST_CASE("A statement stamped before the product existed keeps the lane", "[lane][migration]") {
    // A device without an RTC stamps 1970 (or its build date) until NTP
    // reaches it. Ordering a foreign record against such a stamp would let
    // even a stale record beat a newer user edit, so an unknowable order
    // means the record does not displace.
    using helix::ams::outside_edit_wins;

    helix::ams::FilamentSlotOverride record;
    record.updated_at = std::chrono::system_clock::time_point{std::chrono::seconds(1790337600)};
    helix::ams::Observation standing(ObservationSource::LocalUser);

    // The unread clock: the order is unknowable, so the statement stays.
    standing.edited_at = std::chrono::system_clock::time_point{std::chrono::seconds(86400)};
    CHECK_FALSE(outside_edit_wins(record, standing));

    // A readable clock keeps the ordinary rule in both directions.
    standing.edited_at = std::chrono::system_clock::time_point{std::chrono::seconds(1790341200)};
    CHECK_FALSE(outside_edit_wins(record, standing));
    standing.edited_at = std::chrono::system_clock::time_point{std::chrono::seconds(1790334000)};
    CHECK(outside_edit_wins(record, standing));
}

TEST_CASE("Colour and material answer through the load rule in both wire formats",
          "[lane][migration]") {
    // lane_data is a shared namespace and spells the lock keys helix_locked_*;
    // the private cache spells them bare. Either way the parser reads them into
    // the declared set, and a record can declare one field and merely remember
    // the other.
    using helix::ams::from_json;
    using helix::ams::from_lane_data_record;
    using helix::ams::sources_from_record;
    using helix::ams::to_json;

    SECTION("the shared lane_data record") {
        const nlohmann::json wire{{"lane", 0},
                                  {"color", "#3355FF"},
                                  {"helix_material", "PETG"},
                                  {"helix_locked_color", true},
                                  {"helix_locked_material", false}};
        const auto parsed = from_lane_data_record(wire);
        REQUIRE(parsed.has_value());

        const auto sources = sources_from_record(parsed->second, wire, LegacyLockKeys::LaneData);
        REQUIRE(sources.local_user.has_value());
        REQUIRE(sources.local_user->color_rgb.has_value());
        CHECK(*sources.local_user->color_rgb == 0x3355FFu);
        CHECK_FALSE(sources.local_user->material.has_value());
        REQUIRE(sources.remembered.has_value());
        REQUIRE(sources.remembered->material.has_value());
        CHECK(*sources.remembered->material == "PETG");
    }

    SECTION("the private cache record") {
        helix::ams::FilamentSlotOverride ovr;
        ovr.color_rgb = 0x3355FF;
        ovr.color_set = true;
        ovr.material = "PETG";
        ovr.declared = helix::ams::declared_fields_from_names(nlohmann::json::array({"color_rgb"}));

        const nlohmann::json wire = to_json(ovr);
        const auto sources = sources_from_record(from_json(wire), wire, LegacyLockKeys::LocalCache);
        REQUIRE(sources.local_user.has_value());
        REQUIRE(sources.local_user->color_rgb.has_value());
        CHECK(*sources.local_user->color_rgb == 0x3355FFu);
        CHECK_FALSE(sources.local_user->material.has_value());
        REQUIRE(sources.remembered.has_value());
        REQUIRE(sources.remembered->material.has_value());
        CHECK(*sources.remembered->material == "PETG");
    }
}

TEST_CASE("A declared brand survives the private cache round-trip", "[lane][migration]") {
    // The local cache is the other document a record goes home in, so it
    // carries the declared set under its own bare key.
    const helix::SlotInfo empty_lane;
    helix::SlotInfo edited;
    edited.brand = "Hatchbox";
    const helix::ams::FilamentSlotOverride ovr =
        helix::ams::user_override_from_slot_info(empty_lane, edited, nullptr);

    const nlohmann::json wire = helix::ams::to_json(ovr);
    REQUIRE(wire.contains("declared"));

    const auto sources = helix::ams::sources_from_record(helix::ams::from_json(wire), wire,
                                                         LegacyLockKeys::LocalCache);
    REQUIRE(sources.local_user.has_value());
    REQUIRE(sources.local_user->brand.has_value());
    CHECK(*sources.local_user->brand == "Hatchbox");
    CHECK_FALSE(sources.remembered.has_value());
}

TEST_CASE("The declared set carries colour and material", "[lane][migration]") {
    // Colour and material keep their authorship in the declared set beside
    // brand, spool name and vendor id. A name in helix_declared declares the
    // field whatever its lock key says, since the key is only written from the
    // set.
    using helix::ams::from_lane_data_record;
    using helix::ams::sources_from_record;
    using helix::ams::to_json;

    SECTION("a record naming them in helix_declared gets them declared") {
        const nlohmann::json wire{{"lane", 0},
                                  {"color", "#3355FF"},
                                  {"helix_material", "PETG"},
                                  {"vendor", "Hatchbox"},
                                  {"helix_locked_color", false},
                                  {"helix_locked_material", false},
                                  {"helix_declared", {"brand", "material", "color_rgb"}}};
        const auto parsed = from_lane_data_record(wire);
        REQUIRE(parsed.has_value());

        const auto sources = sources_from_record(parsed->second, wire, LegacyLockKeys::LaneData);

        // All three answer from the set and are the user's.
        REQUIRE(sources.local_user.has_value());
        REQUIRE(sources.local_user->brand.has_value());
        CHECK(*sources.local_user->brand == "Hatchbox");
        REQUIRE(sources.local_user->material.has_value());
        CHECK(*sources.local_user->material == "PETG");
        REQUIRE(sources.local_user->color_rgb.has_value());
        CHECK(*sources.local_user->color_rgb == 0x3355FFu);
        CHECK_FALSE(sources.remembered.has_value());

        // The set took all three names. Re-emitting it is what shows that: the
        // emitter mirrors the set without a filter of its own, and writes the
        // lock keys from it.
        const nlohmann::json reemitted = to_json(parsed->second);
        CHECK(reemitted["declared"] == nlohmann::json::array({"color_rgb", "material", "brand"}));
        CHECK(reemitted["user_locked_color"] == true);
        CHECK(reemitted["user_locked_material"] == true);
    }

    SECTION("an edit that supplies all three names all three in the set") {
        const helix::SlotInfo empty_lane;
        helix::SlotInfo edited;
        edited.brand = "Hatchbox";
        edited.material = "PETG";
        edited.color_rgb = 0x3355FF;
        const auto ovr = helix::ams::user_override_from_slot_info(empty_lane, edited, nullptr);

        CHECK(helix::ams::declares_color(ovr));
        CHECK(helix::ams::declares_material(ovr));
        CHECK(to_json(ovr)["declared"] ==
              nlohmann::json::array({"color_rgb", "material", "brand"}));
    }
}

// ============================================================================
// What a user's edit claims, end to end: user_override_from_slot_info builds the
// record, the store emits it, and sources_from_record routes it back.
// ============================================================================

namespace {

/// A lane carrying only what the machine reported, which is what the spool
/// editor seeds its working copy from.
helix::SlotInfo firmware_lane() {
    helix::SlotInfo info;
    info.brand = "Firmware Brand";
    info.spool_name = "Firmware Spool";
    info.spoolman_vendor_id = 3;
    info.material = "PLA";
    info.color_rgb = 0x3355FF;
    return info;
}

/// Persist @p ovr the way a backend does and put the reloaded record into the
/// lane model, exactly as a backend's on_started() would.
helix::ams::LaneId reload_into_lane(FilamentSlotOverrideStore& store,
                                    const helix::ams::FilamentSlotOverride& ovr) {
    bool saved = false;
    store.save_async(0, ovr, [&](bool, std::string) { saved = true; });
    REQUIRE(saved);
    REQUIRE(store.load_blocking().size() == 1);
    REQUIRE(ingest_legacy_records(store, LegacyLockKeys::LaneData, /*backend_index=*/0) == 1);
    return lane_id_for(0, 0);
}

/// Everything the machine states on a later frame, all of it different from
/// what firmware_lane() holds.
Observation correcting_frame() {
    Observation frame(ObservationSource::VendorCache);
    frame.brand = "Corrected Brand";
    frame.spool_name = "Corrected Spool";
    frame.spoolman_vendor_id = 9;
    frame.material = "PETG";
    frame.color_rgb = 0xFF0000;
    return frame;
}

} // namespace

TEST_CASE_METHOD(HelixTestFixture,
                 "An edit that moves only the weight claims none of the identity it carried",
                 "[lane][migration]") {
    // The editor opens on the lane, so firmware's brand, spool name, vendor id,
    // colour and material all come back on the commit untouched. A record
    // claiming them would outrank the machine that supplied them, and no later
    // firmware correction could ever land.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ad5x_ifs");

    const helix::SlotInfo before = firmware_lane();
    helix::SlotInfo edited = before;
    edited.remaining_weight_g = 730.0F;

    const auto ovr = helix::ams::user_override_from_slot_info(before, edited, nullptr);
    CHECK_FALSE(helix::ams::declares_color(ovr));
    CHECK_FALSE(helix::ams::declares_material(ovr));
    CHECK_FALSE(ovr.declared.any());
    // The identity still travels: the lane has to show it. Only the claim on it
    // does not.
    CHECK(ovr.brand == "Firmware Brand");
    CHECK(ovr.spool_name == "Firmware Spool");
    CHECK(ovr.material == "PLA");
    CHECK(ovr.color_set);

    const helix::ams::LaneId lane = reload_into_lane(store, ovr);
    const auto sources = lane_sources(lane);
    CHECK_FALSE(sources.local_user.has_value());
    REQUIRE(sources.remembered.has_value());
    CHECK(sources.remembered->brand == "Firmware Brand");
    CHECK(sources.remembered->spool_name == "Firmware Spool");
    CHECK(sources.remembered->material == "PLA");

    ingest(lane, correcting_frame());
    const auto resolved = resolved_lane(lane);
    CHECK(resolved.brand == "Corrected Brand");
    CHECK(resolved.spool_name == "Corrected Spool");
    CHECK(resolved.spoolman_vendor_id == 9);
    CHECK(resolved.material == "PETG");
    CHECK(resolved.color_rgb == 0xFF0000u);
}

TEST_CASE_METHOD(HelixTestFixture, "An edit that moves the brand claims the brand alone",
                 "[lane][migration]") {
    // One record, three fields whose authorship rides the declared set, and
    // exactly one of them moved. The record carries all three, so the routing
    // has to split them rather than answer once for the record.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ad5x_ifs");

    const helix::SlotInfo before = firmware_lane();
    helix::SlotInfo edited = before;
    edited.brand = "Hatchbox";

    const auto ovr = helix::ams::user_override_from_slot_info(before, edited, nullptr);
    CHECK_FALSE(helix::ams::declares_color(ovr));
    CHECK_FALSE(helix::ams::declares_material(ovr));
    const nlohmann::json declared = helix::ams::declared_field_names(ovr.declared);
    REQUIRE(declared.is_array());
    CHECK(declared.size() == 1);
    CHECK(declared.at(0) == "brand");

    const helix::ams::LaneId lane = reload_into_lane(store, ovr);
    const auto sources = lane_sources(lane);
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->brand == "Hatchbox");
    CHECK_FALSE(sources.local_user->spool_name.has_value());
    CHECK_FALSE(sources.local_user->spoolman_vendor_id.has_value());
    REQUIRE(sources.remembered.has_value());
    CHECK(sources.remembered->spool_name == "Firmware Spool");
    CHECK(sources.remembered->spoolman_vendor_id == 3);

    ingest(lane, correcting_frame());
    const auto resolved = resolved_lane(lane);
    CHECK(resolved.brand == "Hatchbox");
    CHECK(resolved.spool_name == "Corrected Spool");
    CHECK(resolved.spoolman_vendor_id == 9);
    CHECK(resolved.material == "PETG");
    CHECK(resolved.color_rgb == 0xFF0000u);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "An edit that clears a field yields it back to the machine after a restart",
                 "[lane][migration][1661]") {
    // A clear means "I don't know", not "this lane has none": no field's clear
    // is a durable declaration, so nothing may record one. The record the
    // clear leaves behind declares nothing, and the value the next firmware
    // frame states lands on the lane again after a reload. Material rides the
    // same test to pin the half that already behaved this way
    // (prestonbrown/helixscreen#1661).
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ad5x_ifs");

    const helix::SlotInfo before = firmware_lane();
    helix::SlotInfo edited = before;

    const auto cleared = [&]() {
        const auto ovr = helix::ams::user_override_from_slot_info(before, edited, nullptr);
        CHECK_FALSE(ovr.declared.any());

        const helix::ams::LaneId lane = reload_into_lane(store, ovr);
        CHECK_FALSE(lane_sources(lane).local_user.has_value());

        ingest(lane, correcting_frame());
        return resolved_lane(lane);
    };

    SECTION("brand") {
        edited.brand.clear();
        CHECK(cleared().brand == "Corrected Brand");
    }
    SECTION("spool name") {
        edited.spool_name.clear();
        CHECK(cleared().spool_name == "Corrected Spool");
    }
    SECTION("vendor id") {
        edited.spoolman_vendor_id = 0;
        CHECK(cleared().spoolman_vendor_id == 9);
    }
    SECTION("material") {
        edited.material.clear();
        CHECK(cleared().material == "PETG");
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "a stored record declaring a field it holds nothing in loads as undeclared",
                 "[lane][migration][1661]") {
    // A record already on a user's printer can name a field in its declared set
    // that it carries no value for, written by a build that recorded clears. A
    // declaration stands over a value, so the name reads as no declaration
    // rather than as an error or a durable empty, and the machine's value
    // lands on the lane.
    using helix::ams::from_lane_data_record;
    using helix::ams::sources_from_record;

    const nlohmann::json wire{{"lane", 0},
                              {"spool_name", "Bench Spool"},
                              {"helix_declared", nlohmann::json::array({"brand", "spool_name"})}};
    const auto parsed = from_lane_data_record(wire);
    REQUIRE(parsed.has_value());

    const auto sources = sources_from_record(parsed->second, wire, LegacyLockKeys::LaneData);
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spool_name == "Bench Spool");
    CHECK_FALSE(sources.local_user->brand.has_value());

    const helix::ams::LaneId lane = lane_id_for(0, 0);
    CHECK(file_lane_sources(lane, sources));
    Observation frame(ObservationSource::VendorCache);
    frame.brand = "Firmware Brand";
    ingest(lane, frame);
    CHECK(resolved_lane(lane).brand == "Firmware Brand");
}

TEST_CASE_METHOD(HelixTestFixture, "An edit that moves the material locks it against the machine",
                 "[lane][migration]") {
    // The #965 protection: a material the person moved is theirs, and no
    // firmware frame may take it back.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ad5x_ifs");

    const helix::SlotInfo before = firmware_lane();
    helix::SlotInfo edited = before;
    edited.material = "ASA";

    const auto ovr = helix::ams::user_override_from_slot_info(before, edited, nullptr);
    CHECK(helix::ams::declared_field_names(ovr.declared) == nlohmann::json::array({"material"}));

    const helix::ams::LaneId lane = reload_into_lane(store, ovr);
    REQUIRE(lane_sources(lane).local_user.has_value());
    CHECK(lane_sources(lane).local_user->material == "ASA");

    ingest(lane, correcting_frame());
    const auto resolved = resolved_lane(lane);
    CHECK(resolved.material == "ASA");
    // The colour rode along on the same commit and was never moved, so the
    // machine still owns it.
    CHECK(resolved.color_rgb == 0xFF0000u);
}

TEST_CASE_METHOD(HelixTestFixture, "A later edit keeps what an earlier edit declared",
                 "[lane][migration]") {
    // An edit speaks about the fields it moved and says nothing about the
    // rest. The colour below was chosen in the first edit and never mentioned
    // again, so a record built from the second edit alone would hand it back
    // to the machine while the lane still calls it the user's.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ad5x_ifs");

    const helix::SlotInfo before = firmware_lane();
    helix::SlotInfo chose_colour = before;
    chose_colour.color_rgb = 0x1E5AA8;
    const auto first = helix::ams::user_override_from_slot_info(before, chose_colour, nullptr);
    REQUIRE(helix::ams::declared_field_names(first.declared) ==
            nlohmann::json::array({"color_rgb"}));

    // The editor re-opens on the lane the first edit left behind, and this
    // time only the brand moves.
    helix::SlotInfo chose_brand = chose_colour;
    chose_brand.brand = "Hatchbox";
    const auto second = helix::ams::user_override_from_slot_info(chose_colour, chose_brand, &first);

    CHECK(second.color_rgb == 0x1E5AA8u);
    CHECK(helix::ams::declared_field_names(second.declared) ==
          nlohmann::json::array({"color_rgb", "brand"}));

    // Both come back as the user's word after a restart, not as something the
    // record merely remembered.
    const helix::ams::LaneId lane = reload_into_lane(store, second);
    const auto sources = lane_sources(lane);
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->brand == "Hatchbox");
    REQUIRE(sources.local_user->color_rgb.has_value());
    CHECK(*sources.local_user->color_rgb == 0x1E5AA8u);

    ingest(lane, correcting_frame());
    const auto resolved = resolved_lane(lane);
    CHECK(resolved.brand == "Hatchbox");
    CHECK(resolved.color_rgb == 0x1E5AA8u);
    // The fields neither edit moved are still the machine's to correct.
    CHECK(resolved.material == "PETG");
    CHECK(resolved.spool_name == "Corrected Spool");
}

TEST_CASE_METHOD(HelixTestFixture, "Linking a spool declares the binding, not what rode in with it",
                 "[lane][migration]") {
    // Picking a spool fills the editor with the server's brand, name and
    // colour. The person chose a spool, not any of those values.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ad5x_ifs");

    const helix::SlotInfo before;
    helix::SlotInfo edited;
    edited.spoolman_id = 42;
    edited.brand = "Hatchbox";
    edited.spool_name = "Blue PETG 1kg";
    edited.spoolman_vendor_id = 7;
    edited.material = "PETG";
    edited.color_rgb = 0x3355FF;

    const auto ovr = helix::ams::user_override_from_slot_info(before, edited, nullptr);
    CHECK(ovr.spoolman_id == 42);
    CHECK_FALSE(helix::ams::declares_color(ovr));
    CHECK_FALSE(helix::ams::declares_material(ovr));
    CHECK_FALSE(ovr.declared.any());

    // A linked record is wholly the server's on reload, which is the same
    // answer by a different route, so the lane names Spoolman and no user rung.
    const helix::ams::LaneId lane = reload_into_lane(store, ovr);
    const auto sources = lane_sources(lane);
    CHECK_FALSE(sources.local_user.has_value());
    REQUIRE(sources.spoolman.has_value());
    CHECK(sources.spoolman->spoolman_id == 42);
    CHECK(sources.spoolman->brand == "Hatchbox");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "a legacy record whose true lock key stands over no colour does not declare its "
                 "brand",
                 "[lane][migration]") {
    // A declaration needs a value to stand over, so a true colour key beside no
    // colour declares no colour, and a record with no declared set then has no
    // evidence a person edited its brand.
    using helix::ams::declares_color;
    using helix::ams::from_lane_data_record;
    using helix::ams::sources_from_record;

    const nlohmann::json wire{{"lane", 0}, {"vendor", "Hatchbox"}, {"helix_locked_color", true}};
    const auto parsed = from_lane_data_record(wire);
    REQUIRE(parsed.has_value());
    CHECK_FALSE(declares_color(parsed->second));

    const auto sources = sources_from_record(parsed->second, wire, LegacyLockKeys::LaneData);
    CHECK_FALSE(sources.local_user.has_value());
    REQUIRE(sources.remembered.has_value());
    CHECK(sources.remembered->brand == "Hatchbox");

    const helix::ams::LaneId lane = lane_id_for(0, 0);
    CHECK(file_lane_sources(lane, sources));
    Observation frame(ObservationSource::VendorCache);
    frame.brand = "Firmware Brand";
    ingest(lane, frame);
    CHECK(resolved_lane(lane).brand == "Firmware Brand");
}
