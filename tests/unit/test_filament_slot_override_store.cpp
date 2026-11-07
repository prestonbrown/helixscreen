// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The lane record store: RFC 0001 §5 records in Moonraker's database, read
// whole and written through, with no local copy.
#include "../test_helpers/mock_printer.h"
#include "ams_types.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"

#include <mutex>
#include <string>
#include <unordered_map>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ams::FilamentSlotOverride;
using helix::ams::FilamentSlotOverrideStore;
using helix::ams::LaneKeyStyle;
using nlohmann::json;

namespace {

FilamentSlotOverride polymaker_pla() {
    FilamentSlotOverride o;
    o.brand = "Polymaker";
    o.spool_name = "PolyTerra";
    o.material = "PLA";
    o.color_rgb = 0xFF5500;
    o.color_set = true;
    o.color_name = "Sunrise";
    o.spoolman_id = 42;
    o.spoolman_filament_id = 55;
    o.remaining_weight_g = 850.0f;
    o.total_weight_g = 1000.0f;
    o.catalog_id = "polymaker-polyterra-pla";
    o.product_name = "PolyTerra PLA";
    o.loaded = true;
    return o;
}

/// Run @p store's reload and return what it handed back.
std::unordered_map<int, helix::ams::LaneDataRecord> read_back(FilamentSlotOverrideStore& store) {
    std::unordered_map<int, helix::ams::LaneDataRecord> out;
    bool called = false;
    store.reload_async([&](std::unordered_map<int, helix::ams::LaneDataRecord> records) {
        out = std::move(records);
        called = true;
    });
    REQUIRE(called);
    return out;
}

} // namespace

// ============================================================================
// Record shape (RFC 0001 §5)
// ============================================================================

TEST_CASE("a record HelixScreen writes carries the RFC 0001 keys", "[filament_slot_override]") {
    const json j = helix::ams::to_lane_data_record(2, polymaker_pla());
    CHECK(j["lss_version"] == "0.1");
    CHECK(j["writer"] == "helixscreen");
    CHECK(j["lane"] == "2");
    CHECK(j["lane_name"] == "lane3");
    CHECK(j["loaded"] == true);
    CHECK(j["color"] == "#FF5500");
    CHECK(j["material"] == "PLA");
    CHECK(j["filament_type"] == "PLA");
    CHECK(j["vendor_name"] == "Polymaker");
    CHECK(j["name"] == "PolyTerra");
    CHECK(j["spool_id"] == 42);
    CHECK(j["spoolman_filament_id"] == 55);
    CHECK(j["remaining_weight_g"] == 850.0f);
    CHECK(j["total_weight_g"] == 1000.0f);
    CHECK(j["nozzle_temp"].is_number());
    CHECK(j["bed_temp"].is_number());
    CHECK(j["helix_color_name"] == "Sunrise");
    CHECK(j["helix_catalog_id"] == "polymaker-polyterra-pla");
    CHECK(j["helix_product_name"] == "PolyTerra PLA");
    // Spellings only older readers used are not written.
    CHECK_FALSE(j.contains("vendor"));
    CHECK_FALSE(j.contains("spool_name"));
    CHECK_FALSE(j.contains("helix_material"));
    CHECK_FALSE(j.contains("helix_declared"));
    CHECK_FALSE(j.contains("helix_locked_color"));
}

TEST_CASE("a record states unknown as null, never a placeholder", "[filament_slot_override]") {
    const json j = helix::ams::to_lane_data_record(0, FilamentSlotOverride{});
    for (const char* key : {"loaded", "color", "material", "filament_type", "vendor_name", "name",
                            "spool_id", "spoolman_filament_id", "filament_id", "nozzle_temp",
                            "bed_temp", "remaining_weight_g", "total_weight_g"}) {
        INFO(key);
        REQUIRE(j.contains(key));
        CHECK(j[key].is_null());
    }
    // HelixScreen's own keys are left out rather than nulled.
    CHECK_FALSE(j.contains("helix_color_name"));
    CHECK_FALSE(j.contains("helix_fingerprint"));
}

TEST_CASE("a tool changer's record names its lane by its T key", "[filament_slot_override]") {
    const json j = helix::ams::to_lane_data_record(1, FilamentSlotOverride{}, LaneKeyStyle::Tool);
    CHECK(j["lane"] == "1");
    CHECK(j["lane_name"] == "T1");
}

TEST_CASE("a record reads back as what was written", "[filament_slot_override]") {
    const FilamentSlotOverride o = polymaker_pla();
    const auto parsed = helix::ams::from_lane_data_record(helix::ams::to_lane_data_record(0, o));
    REQUIRE(parsed.has_value());
    const FilamentSlotOverride& r = parsed->second;
    CHECK(r.brand == o.brand);
    CHECK(r.spool_name == o.spool_name);
    CHECK(r.material == o.material);
    CHECK(r.color_set);
    CHECK(r.color_rgb == o.color_rgb);
    CHECK(r.color_name == o.color_name);
    CHECK(r.spoolman_id == o.spoolman_id);
    CHECK(r.spoolman_filament_id == o.spoolman_filament_id);
    CHECK(r.catalog_id == o.catalog_id);
    CHECK(r.product_name == o.product_name);
    CHECK(r.loaded == std::optional<bool>(true));
}

TEST_CASE("a record with no writer still reads, by its legacy spellings",
          "[filament_slot_override]") {
    const json legacy = {{"lane", "1"},
                         {"vendor", "Hatchbox"},
                         {"spool_name", "Basic"},
                         {"helix_material", "PETG-CF"},
                         {"material", "PETG"},
                         {"helix_spoolman_filament_id", 9},
                         {"spoolman_vendor_id", 4},
                         {"color_name", "Ink"},
                         {"color", "#112233"}};
    const auto parsed = helix::ams::from_lane_data_record(legacy);
    REQUIRE(parsed.has_value());
    CHECK(parsed->first == 1);
    const FilamentSlotOverride& r = parsed->second;
    CHECK(r.brand == "Hatchbox");
    CHECK(r.spool_name == "Basic");
    CHECK(r.material == "PETG-CF");
    CHECK(r.spoolman_filament_id == 9);
    CHECK(r.spoolman_vendor_id == 4);
    CHECK(r.color_name == "Ink");
    CHECK(r.color_rgb == 0x112233);
    CHECK_FALSE(r.loaded.has_value());
}

TEST_CASE("a numeric filament_id is Spoolman's filament id", "[filament_slot_override]") {
    // Happy Hare writes Spoolman's filament id there (RFC 0001 §7).
    const auto parsed = helix::ams::from_lane_data_record(json{{"lane", "0"}, {"filament_id", 17}});
    REQUIRE(parsed.has_value());
    CHECK(parsed->second.spoolman_filament_id == 17);
    CHECK(parsed->second.spoolman_orca_filament_id.empty());
}

TEST_CASE("another tool's matchable material reads when no precise type is stated",
          "[filament_slot_override]") {
    const auto parsed = helix::ams::from_lane_data_record(json{{"lane", "0"}, {"material", "ABS"}});
    REQUIRE(parsed.has_value());
    CHECK(parsed->second.material == "ABS");
}

TEST_CASE("a material Orca cannot match leaves material null and keeps the precise type",
          "[filament_slot_override]") {
    FilamentSlotOverride o;
    o.material = "PPS-CF";
    const json j = helix::ams::to_lane_data_record(0, o);
    CHECK(j["material"].is_null());
    CHECK(j["filament_type"] == "PPS-CF");
}

TEST_CASE("a lane record files whole as the Record source", "[filament_slot_override][lane]") {
    const helix::ams::Observation obs = helix::ams::record_observation(polymaker_pla());
    CHECK(obs.source == helix::ams::ObservationSource::Record);
    CHECK(obs.brand == std::optional<std::string>("Polymaker"));
    CHECK(obs.spoolman_id == std::optional<int>(42));
    CHECK(obs.remaining_weight_g == std::optional<float>(850.0f));

    // Unknown is unobserved, so it states nothing over another source.
    const helix::ams::Observation blank = helix::ams::record_observation(FilamentSlotOverride{});
    CHECK_FALSE(blank.brand.has_value());
    CHECK_FALSE(blank.color_rgb.has_value());
    CHECK_FALSE(blank.spoolman_id.has_value());
    CHECK_FALSE(blank.remaining_weight_g.has_value());
}

// ============================================================================
// Writing and reading the database
// ============================================================================

TEST_CASE("a save writes the record under the slot's key with a scan time",
          "[filament_slot_override]") {
    MockPrinter printer;
    FilamentSlotOverrideStore store(&printer.api, "ifs");
    bool ok = false;
    store.save_async(0, polymaker_pla(), [&](bool success, std::string) { ok = success; });
    REQUIRE(ok);
    const json stored = printer.client.mock_db_get("lane_data", "lane1");
    REQUIRE(stored.is_object());
    CHECK(stored["writer"] == "helixscreen");
    CHECK(stored["vendor_name"] == "Polymaker");
    CHECK(stored["scan_time"].is_string());
}

TEST_CASE("a save the database refuses reports an error and writes nothing",
          "[filament_slot_override]") {
    MockPrinter printer;
    FilamentSlotOverrideStore store(&printer.api, "ifs");
    printer.client.fail_next("server.database.post_item");
    bool done = false;
    bool ok = true;
    store.save_async(0, polymaker_pla(), [&](bool success, std::string) {
        ok = success;
        done = true;
    });
    REQUIRE(done);
    CHECK_FALSE(ok);
    CHECK(printer.client.mock_db_get("lane_data", "lane1").is_null());
}

TEST_CASE("a clear deletes the slot's record and leaves the others", "[filament_slot_override]") {
    MockPrinter printer;
    FilamentSlotOverrideStore store(&printer.api, "ifs");
    store.save_async(0, polymaker_pla(), nullptr);
    store.save_async(1, polymaker_pla(), nullptr);
    bool ok = false;
    store.clear_async(0, [&](bool success, std::string) { ok = success; });
    CHECK(ok);
    CHECK(printer.client.mock_db_get("lane_data", "lane1").is_null());
    CHECK_FALSE(printer.client.mock_db_get("lane_data", "lane2").is_null());
}

TEST_CASE("a write that lands tells the store's owner to read again", "[filament_slot_override]") {
    MockPrinter printer;
    FilamentSlotOverrideStore store(&printer.api, "ifs");
    int written = 0;
    store.set_on_written([&written]() { ++written; });
    store.save_async(0, polymaker_pla(), nullptr);
    CHECK(written == 1);
    store.clear_async(0, nullptr);
    CHECK(written == 2);
    printer.client.fail_next("server.database.post_item");
    store.save_async(0, polymaker_pla(), nullptr);
    CHECK(written == 2);
}

TEST_CASE("a read hands back every record, matched by its lane field", "[filament_slot_override]") {
    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "gate_0", json{{"lane", "3"}, {"material", "ABS"}});
    printer.client.mock_db_set("lane_data", "seated", json(2));
    FilamentSlotOverrideStore store(&printer.api, "ace");
    const auto records = read_back(store);
    REQUIRE(records.size() == 1);
    REQUIRE(records.count(3) == 1);
    CHECK(records.at(3).record.material == "ABS");
}

TEST_CASE("two records naming one slot: the one under our key wins", "[filament_slot_override]") {
    const json doc = {{"T0", {{"lane", "0"}, {"material", "PLA"}}},
                      {"lane1", {{"lane", "0"}, {"material", "PETG"}}}};
    auto tool = helix::ams::parse_namespace_document(doc, LaneKeyStyle::Tool);
    REQUIRE(tool.count(0) == 1);
    CHECK(tool.at(0).record.material == "PLA");
    auto lane = helix::ams::parse_namespace_document(doc, LaneKeyStyle::Lane);
    REQUIRE(lane.count(0) == 1);
    CHECK(lane.at(0).record.material == "PETG");
}

TEST_CASE("a namespace nothing has written reads as empty", "[filament_slot_override]") {
    MockPrinter printer;
    FilamentSlotOverrideStore store(&printer.api, "ifs");
    CHECK(read_back(store).empty());
}

TEST_CASE("a read the database refuses calls the failure path", "[filament_slot_override]") {
    MockPrinter printer;
    FilamentSlotOverrideStore store(&printer.api, "ifs");
    printer.client.fail_next("server.database.get_item");
    int read = 0;
    int failed = 0;
    store.reload_async([&](std::unordered_map<int, helix::ams::LaneDataRecord>) { ++read; },
                       [&]() { ++failed; });
    CHECK(read == 0);
    CHECK(failed == 1);
}

// ============================================================================
// The writer rule (RFC 0001 S2)
// ============================================================================

TEST_CASE("a record another producer writes is never overwritten",
          "[filament_slot_override][writer_rule]") {
    MockPrinter printer;
    const json theirs = {{"lane", "0"}, {"writer", "openams_spoolman"}, {"spool_id", 7}};
    printer.client.mock_db_set("lane_data", "lane1", theirs);
    FilamentSlotOverrideStore store(&printer.api, "openams");
    store.adopt_read(read_back(store));

    bool ok = true;
    std::string error;
    store.save_async(0, polymaker_pla(), [&](bool success, std::string err) {
        ok = success;
        error = std::move(err);
    });
    CHECK_FALSE(ok);
    CHECK(error.find("openams_spoolman") != std::string::npos);
    CHECK(printer.client.mock_db_get("lane_data", "lane1") == theirs);

    ok = true;
    store.clear_async(0, [&](bool success, std::string) { ok = success; });
    CHECK_FALSE(ok);
    CHECK(printer.client.mock_db_get("lane_data", "lane1") == theirs);
}

TEST_CASE("a legacy record and one of our own are ours to replace",
          "[filament_slot_override][writer_rule]") {
    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "lane1", json{{"lane", "0"}, {"material", "ABS"}});
    printer.client.mock_db_set("lane_data", "lane2",
                               json{{"lane", "1"}, {"writer", "helixscreen"}});
    FilamentSlotOverrideStore store(&printer.api, "ifs");
    store.adopt_read(read_back(store));
    int saved = 0;
    for (int slot : {0, 1}) {
        store.save_async(slot, polymaker_pla(),
                         [&](bool success, std::string) { saved += success ? 1 : 0; });
    }
    CHECK(saved == 2);
}

// ============================================================================
// Extras namespaces (AFC, Happy Hare)
// ============================================================================

TEST_CASE("an extras namespace stores only what firmware cannot hold",
          "[filament_slot_override][extras]") {
    MockPrinter printer;
    FilamentSlotOverrideStore store(&printer.api, "afc", LaneKeyStyle::Lane,
                                    "helix-screen-afc-overrides");
    REQUIRE(store.holds_extras_only());
    store.save_async(0, polymaker_pla(), nullptr);
    const json stored = printer.client.mock_db_get("helix-screen-afc-overrides", "lane1");
    REQUIRE(stored.is_object());
    CHECK(stored["vendor_name"] == "Polymaker");
    CHECK(stored["name"] == "PolyTerra");
    CHECK(stored["helix_color_name"] == "Sunrise");
    CHECK(stored["helix_catalog_id"] == "polymaker-polyterra-pla");
    CHECK(stored["total_weight_g"] == 1000.0f);
    CHECK(stored["spool_id"].is_null());
    CHECK(stored["material"].is_null());
    CHECK(stored["color"].is_null());
    CHECK(stored["remaining_weight_g"].is_null());
}

TEST_CASE("an extras record's spool id, material and colour are ignored on read",
          "[filament_slot_override][extras]") {
    MockPrinter printer;
    printer.client.mock_db_set("helix-screen-hh-overrides", "lane1",
                               json{{"lane", "0"},
                                    {"spool_id", 12},
                                    {"material", "PETG"},
                                    {"color", "#00FF00"},
                                    {"vendor", "Sunlu"}});
    FilamentSlotOverrideStore store(&printer.api, "happyhare", LaneKeyStyle::Lane,
                                    "helix-screen-hh-overrides");
    const auto records = read_back(store);
    REQUIRE(records.count(0) == 1);
    const FilamentSlotOverride& r = records.at(0).record;
    CHECK(r.brand == "Sunlu");
    CHECK(r.spoolman_id == 0);
    CHECK(r.material.empty());
    CHECK_FALSE(r.color_set);
}

// ============================================================================
// The persist tail
// ============================================================================

TEST_CASE("persist_staged_override saves only a slot that holds a record",
          "[filament_slot_override]") {
    MockPrinter printer;
    FilamentSlotOverrideStore store(&printer.api, "ifs");
    std::mutex mutex;
    std::unordered_map<int, FilamentSlotOverride> overrides;
    overrides[1] = polymaker_pla();
    helix::ams::persist_staged_override(&store, mutex, overrides, 1, "[test]", "Override");
    CHECK_FALSE(printer.client.mock_db_get("lane_data", "lane2").is_null());
    helix::ams::persist_staged_override(&store, mutex, overrides, 0, "[test]", "Override");
    CHECK(printer.client.mock_db_get("lane_data", "lane1").is_null());
}

TEST_CASE("a failed write withdraws the lane's pending edit",
          "[filament_slot_override][lane][pending]") {
    helix::ams::reset_lane_sources();
    MockPrinter printer;
    FilamentSlotOverrideStore store(&printer.api, "ifs");
    const helix::ams::LaneId lane = helix::ams::lane_id_for(0, 0);
    helix::ams::Observation edit(helix::ams::ObservationSource::Pending);
    edit.material = "PETG";
    helix::ams::file_pending(lane, edit);
    REQUIRE(helix::ams::lane_sources(lane).pending.has_value());

    SECTION("a refused write") {
        printer.client.fail_next("server.database.post_item");
        helix::ams::save_override_async(&store, 0, polymaker_pla(), "[test]", "Override", lane);
        CHECK_FALSE(helix::ams::lane_sources(lane).pending.has_value());
    }
    SECTION("a write that lands leaves it for the echo") {
        helix::ams::save_override_async(&store, 0, polymaker_pla(), "[test]", "Override", lane);
        CHECK(helix::ams::lane_sources(lane).pending.has_value());
    }
    helix::ams::reset_lane_sources();
}

// ============================================================================
// Mirror
// ============================================================================

TEST_CASE("FillUnsetOnly fills a blank record and leaves a set one",
          "[filament_slot_override][mirror]") {
    std::unordered_map<int, FilamentSlotOverride> overrides;
    CHECK(helix::ams::mirror_firmware_to_lane_data(nullptr, overrides, 0, 0x000000, "PLA", true,
                                                   helix::ams::MirrorPolicy::FillUnsetOnly,
                                                   "[test]"));
    CHECK(overrides[0].color_set);
    CHECK(overrides[0].color_rgb == 0x000000);
    CHECK(overrides[0].material == "PLA");
    CHECK_FALSE(helix::ams::mirror_firmware_to_lane_data(
        nullptr, overrides, 0, 0x00FF00, "PETG", true, helix::ams::MirrorPolicy::FillUnsetOnly,
        "[test]"));
    CHECK(overrides[0].material == "PLA");
}

TEST_CASE("OverwriteAlways follows firmware, and an empty slot is no signal",
          "[filament_slot_override][mirror]") {
    MockPrinter printer;
    FilamentSlotOverrideStore store(&printer.api, "ifs");
    std::unordered_map<int, FilamentSlotOverride> overrides;
    overrides[0] = polymaker_pla();
    CHECK(helix::ams::mirror_firmware_to_lane_data(&store, overrides, 0, 0x0000FF, "PETG", true,
                                                   helix::ams::MirrorPolicy::OverwriteAlways,
                                                   "[test]"));
    CHECK(overrides[0].material == "PETG");
    CHECK(printer.client.mock_db_get("lane_data", "lane1")["color"] == "#0000FF");
    // Steady state: nothing to write.
    CHECK_FALSE(helix::ams::mirror_firmware_to_lane_data(
        &store, overrides, 0, 0x0000FF, "PETG", true, helix::ams::MirrorPolicy::OverwriteAlways,
        "[test]"));
    CHECK_FALSE(helix::ams::mirror_firmware_to_lane_data(
        &store, overrides, 1, 0x0000FF, "PETG", false, helix::ams::MirrorPolicy::OverwriteAlways,
        "[test]"));
    CHECK(overrides.count(1) == 0);
}

// ============================================================================
// Temperatures and fingerprints
// ============================================================================

TEST_CASE("resolved_temps preserves explicit values, falls back to material DB",
          "[filament_slot_override]") {
    using helix::ams::FilamentSlotOverride;
    using helix::ams::resolved_temps;

    SECTION("explicit values pass through unchanged") {
        FilamentSlotOverride o;
        o.material = "PLA"; // PLA defaults differ; explicit must win
        o.bed_temp = 65;
        o.nozzle_temp = 220;
        auto r = resolved_temps(o);
        CHECK(r.bed_temp == 65);
        CHECK(r.nozzle_temp == 220);
    }

    SECTION("zero fields fall back to material DB") {
        FilamentSlotOverride o;
        o.material = "PETG"; // PETG defaults: bed 80, nozzle ~245
        auto r = resolved_temps(o);
        CHECK(r.bed_temp == 80);
        CHECK(r.nozzle_temp == 245);
    }

    SECTION("partial fields: explicit bed, fallback nozzle") {
        FilamentSlotOverride o;
        o.material = "PETG";
        o.bed_temp = 70; // explicit
        // nozzle_temp = 0 → fallback
        auto r = resolved_temps(o);
        CHECK(r.bed_temp == 70);     // user wins
        CHECK(r.nozzle_temp == 245); // material DB
    }

    SECTION("no material → no fallback, both stay 0") {
        FilamentSlotOverride o;
        // material empty
        auto r = resolved_temps(o);
        CHECK(r.bed_temp == 0);
        CHECK(r.nozzle_temp == 0);
    }

    SECTION("unknown material → no fallback, both stay 0") {
        FilamentSlotOverride o;
        o.material = "ZZZ_NOT_A_REAL_MATERIAL";
        auto r = resolved_temps(o);
        CHECK(r.bed_temp == 0);
        CHECK(r.nozzle_temp == 0);
    }

    SECTION("material change picks up new defaults — no stale carry-over") {
        // The point of resolving at emit time, not stamping the override at
        // set time: changing material from PLA → PETG must reflect PETG's
        // defaults on the next resolved_temps() call. Storing fallback values
        // on the struct would freeze PLA's 60°C with the new PETG material.
        FilamentSlotOverride o;
        o.material = "PLA";
        auto pla = resolved_temps(o);
        CHECK(pla.bed_temp == 60); // PLA default

        o.material = "PETG";
        auto petg = resolved_temps(o);
        CHECK(petg.bed_temp == 80); // PETG default — fresh, not stale PLA
    }
}

TEST_CASE("SlotFingerprintTracker seeds baselines and confirms through a sink",
          "[filament_slot_override][ams]") {
    helix::ams::SlotFingerprintTracker tracker;

    // Seeding fills only a slot with no baseline, never with an empty value.
    tracker.seed_baseline(0, "A");
    CHECK(tracker.baseline(0) == "A");
    tracker.seed_baseline(0, "B"); // a live observation outranks a stored one
    CHECK(tracker.baseline(0) == "A");
    tracker.seed_baseline(1, "");
    CHECK_FALSE(tracker.baseline(1).has_value());

    std::vector<std::pair<int, std::string>> seen;
    tracker.set_baseline_sink(
        [&seen](int slot, const std::string& fp) { seen.emplace_back(slot, fp); });

    using helix::ams::FingerprintEvent;
    CHECK(tracker.observe(1, "X") == FingerprintEvent::Baseline);
    CHECK(tracker.observe(1, "X") == FingerprintEvent::Unchanged);
    tracker.expect_any_of(1, {"Y"});
    CHECK(tracker.observe(1, "Y") == FingerprintEvent::OwnWriteEcho);
    CHECK(tracker.observe(1, "Z") == FingerprintEvent::Changed);
    CHECK(tracker.observe(1, "") == FingerprintEvent::NoSignal);

    // Confirmations only: Baseline, Unchanged, OwnWriteEcho. Changed and
    // NoSignal stay silent.
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == std::make_pair(1, std::string("X")));
    CHECK(seen[1] == std::make_pair(1, std::string("X")));
    CHECK(seen[2] == std::make_pair(1, std::string("Y")));
}

TEST_CASE("SlotFingerprintTracker expectations accumulate across writes",
          "[filament_slot_override][ams]") {
    helix::ams::SlotFingerprintTracker tracker;
    tracker.observe(0, "B");

    // Two writes dispatched before either echo lands: both echo values are
    // ours, so arming the second must not drop the first's expectation.
    tracker.expect_any_of(0, {"E1"});
    tracker.expect_any_of(0, {"E2"});
    REQUIRE(tracker.has_expected(0));

    CHECK(tracker.observe(0, "E1") == helix::ams::FingerprintEvent::OwnWriteEcho);
    CHECK(tracker.has_expected(0));
    CHECK(tracker.observe(0, "E2") == helix::ams::FingerprintEvent::OwnWriteEcho);
    CHECK_FALSE(tracker.has_expected(0));

    // Both echoes consumed: the next change is a swap.
    CHECK(tracker.observe(0, "S") == helix::ams::FingerprintEvent::Changed);

    // Re-registering a value already pending adds nothing: an entry is
    // single-shot per VALUE, so one echo still ends it.
    tracker.expect_any_of(0, {"V"});
    tracker.expect_any_of(0, {"V"});
    CHECK(tracker.observe(0, "V") == helix::ams::FingerprintEvent::OwnWriteEcho);
    CHECK_FALSE(tracker.has_expected(0));

    // A change no write asked for consumes everything pending; the
    // swap-while-in-flight guarantee.
    helix::ams::SlotFingerprintTracker swap;
    swap.observe(1, "B");
    swap.expect_any_of(1, {"X", "Y"});
    CHECK(swap.observe(1, "Z") == helix::ams::FingerprintEvent::Changed);
    CHECK_FALSE(swap.has_expected(1));
}

TEST_CASE("SlotFingerprintTracker forget releases only one write's claims",
          "[filament_slot_override][ams]") {
    using helix::ams::FingerprintEvent;
    helix::ams::SlotFingerprintTracker tracker;
    tracker.observe(0, "B");

    // Write A dispatches; write B fails to dispatch entirely. B's forget
    // must leave A's in-flight echo expected, or A's echo reads as a swap
    // and wipes the edit it was armed to protect.
    tracker.expect_any_of(0, {"A1", "A2"});
    const auto b_staged = tracker.expect_any_of(0, {"B1", "B2"});
    tracker.forget_expected(0, b_staged);
    REQUIRE(tracker.has_expected(0));
    CHECK(tracker.observe(0, "A1") == FingerprintEvent::OwnWriteEcho);
    CHECK(tracker.observe(0, "A2") == FingerprintEvent::OwnWriteEcho);
    CHECK_FALSE(tracker.has_expected(0));

    // A value two in-flight writes both expect survives one writer giving
    // up: each staging holds a claim, and a forget releases only its own.
    tracker.observe(0, "A2");
    tracker.expect_any_of(0, {"V"});
    const auto shared_staged = tracker.expect_any_of(0, {"V", "W"});
    tracker.forget_expected(0, shared_staged);
    CHECK(tracker.observe(0, "V") == FingerprintEvent::OwnWriteEcho);
    // W's only claim left with the forget, so W now reads as external.
    CHECK(tracker.observe(0, "W") == FingerprintEvent::Changed);
    CHECK_FALSE(tracker.has_expected(0));
}
