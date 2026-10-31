// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spool_wizard.h" // For FilamentEntry struct

#include "json_utils.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_spoolman_api.h" // For spoolman_detail::parse_spool_info
#include "printer_state.h"
#include "spoolman_types.h" // For SpoolInfo, VendorInfo, FilamentInfo

#include <algorithm>
#include <functional>
#include <optional>
#include <set>

#include "../catch_amalgamated.hpp"

using namespace helix;
TEST_CASE("SpoolInfo - remaining_percent and is_low", "[filament]") {
    struct Pct {
        double initial, remaining, pct;
    };
    // A zero or negative initial weight reads 0% rather than dividing by it.
    const Pct pcts[] = {{1000, 1000, 100},     {1000, 500, 50}, {1000, 0, 0},  {1000, 850, 85},
                        {750, 500, 66.666666}, {0, 100, 0},     {-100, 50, 0}, {1000, 50, 5}};
    for (const auto& p : pcts) {
        INFO(p.remaining << "g of " << p.initial << "g");
        SpoolInfo spool;
        spool.initial_weight_g = p.initial;
        spool.remaining_weight_g = p.remaining;
        CHECK(spool.remaining_percent() == Catch::Approx(p.pct).margin(0.001));
    }

    // Low means strictly under the threshold, 100g by default.
    struct Low {
        double remaining;
        std::optional<double> threshold;
        bool low;
    };
    const Low lows[] = {{99, std::nullopt, true},
                        {100, std::nullopt, false},
                        {101, std::nullopt, false},
                        {200, 250.0, true},
                        {200, 200.0, false},
                        {200, 150.0, false},
                        {0, std::nullopt, true},
                        {0, 0.0, false},
                        {5, 10.0, true},
                        {5, 5.0, false},
                        {5, 1.0, false}};
    for (const auto& l : lows) {
        INFO(l.remaining << "g against " << l.threshold.value_or(-1));
        SpoolInfo spool;
        spool.remaining_weight_g = l.remaining;
        CHECK((l.threshold ? spool.is_low(*l.threshold) : spool.is_low()) == l.low);
    }
}

TEST_CASE("Spoolman display names read vendor, filament name, material", "[filament]") {
    struct Row {
        std::string vendor, material, name;
        std::string spool, filament; // SpoolInfo's and FilamentInfo's display_name
    };
    const Row rows[] = {
        {"Polymaker", "PLA", "Jet Black", "Polymaker Jet Black PLA", "Polymaker Jet Black PLA"},
        {"eSUN", "PETG", "", "eSUN PETG", "eSUN PETG"},
        {"", "ABS", "Red", "Red ABS", "Red ABS"},
        {"", "TPU", "", "TPU", "TPU"},
        {"", "", "", "Unknown Spool", "Unknown Filament"},
        {"", "", "Blue", "Blue", ""},
        {"Eryone", "Silk PLA", "Gold/Silver/Copper Tri-Color",
         "Eryone Gold/Silver/Copper Tri-Color Silk PLA", ""},
    };
    for (const auto& row : rows) {
        INFO("\"" << row.vendor << "\" \"" << row.material << "\" \"" << row.name << "\"");
        SpoolInfo spool;
        spool.vendor = row.vendor;
        spool.material = row.material;
        spool.filament_name = row.name;
        CHECK(spool.display_name() == row.spool);
        if (!row.filament.empty()) {
            FilamentInfo filament;
            filament.vendor_name = row.vendor;
            filament.material = row.material;
            filament.filament_name = row.name;
            CHECK(filament.display_name() == row.filament);
        }
    }

    VendorInfo vendor;
    CHECK(vendor.display_name() == "Unknown Vendor");
    vendor.name = "Hatchbox";
    CHECK(vendor.display_name() == "Hatchbox");
}

// ============================================================================
// Default records
// ============================================================================

TEST_CASE("Spoolman records start empty, with a 1.75mm filament diameter", "[filament]") {
    const SpoolInfo spool;
    CHECK(spool.id == 0);
    CHECK(spool.vendor_id == 0);
    for (const double v : {spool.remaining_weight_g, spool.remaining_length_m, spool.spool_weight_g,
                           spool.initial_weight_g, spool.price})
        CHECK(v == 0.0);
    for (const int t : {spool.nozzle_temp_min, spool.nozzle_temp_max, spool.nozzle_temp_recommended,
                        spool.bed_temp_min, spool.bed_temp_max, spool.bed_temp_recommended})
        CHECK(t == 0);
    for (const auto* s : {&spool.vendor, &spool.material, &spool.filament_name, &spool.color_hex,
                          &spool.lot_nr, &spool.comment, &spool.location})
        CHECK(s->empty());

    const FilamentUsageRecord record;
    CHECK(record.spool_id == 0);
    CHECK(record.used_weight_g == 0.0);
    CHECK(record.used_length_m == 0.0);
    CHECK(record.print_filename.empty());
    CHECK(record.timestamp == 0.0);

    const VendorInfo vendor;
    CHECK(vendor.id == 0);
    CHECK(vendor.name.empty());

    const FilamentInfo filament;
    CHECK(filament.id == 0);
    CHECK(filament.vendor_id == 0);
    CHECK(filament.diameter == Catch::Approx(1.75f));
    for (const float v : {filament.density, filament.weight, filament.spool_weight})
        CHECK(v == 0.0f);
    for (const int t : {filament.nozzle_temp_min, filament.nozzle_temp_max, filament.bed_temp_min,
                        filament.bed_temp_max})
        CHECK(t == 0);
    for (const auto* s :
         {&filament.vendor_name, &filament.material, &filament.filament_name, &filament.color_hex})
        CHECK(s->empty());

    const SpoolWizardOverlay::FilamentEntry entry;
    CHECK(entry.diameter == Catch::Approx(1.75));
    CHECK(entry.density == 0.0);

    CHECK(SlotInfo{}.spoolman_vendor_id == 0);
}

// ============================================================================
// MoonrakerAPIMock Spoolman Tests
// ============================================================================

TEST_CASE("MoonrakerAPIMock - get_spoolman_status", "[filament][mock]") {
    // Create mock client and state
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SECTION("Returns connected by default") {
        bool callback_called = false;
        api.spoolman().get_spoolman_status(
            [&](bool connected, int active_spool_id) {
                callback_called = true;
                REQUIRE(connected == true);
                REQUIRE(active_spool_id == 1); // Default active spool
            },
            [](const MoonrakerError&) { FAIL("Error callback should not be called"); });

        REQUIRE(callback_called);
    }

    SECTION("Can be disabled") {
        client.set_mock_spoolman_enabled(false);

        bool callback_called = false;
        api.spoolman().get_spoolman_status(
            [&](bool connected, int /*active_spool_id*/) {
                callback_called = true;
                REQUIRE(connected == false);
                // active_spool_id still returns the cached value
            },
            [](const MoonrakerError&) {});

        REQUIRE(callback_called);
    }
}

TEST_CASE("MoonrakerAPIMock - get_spoolman_spools", "[filament][mock]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SECTION("Returns non-empty spool list") {
        bool callback_called = false;
        api.spoolman().get_spoolman_spools(
            [&](const std::vector<SpoolInfo>& spools) {
                callback_called = true;
                REQUIRE(spools.size() == 19); // Mock has 19 spools
            },
            [](const MoonrakerError&) { FAIL("Error callback should not be called"); });

        REQUIRE(callback_called);
    }

    SECTION("One listed spool is active by default") {
        int active = 0;
        api.spoolman().get_spoolman_status([&](bool, int id) { active = id; }, nullptr);
        bool listed = false;
        api.spoolman().get_spoolman_spools(
            [&](const std::vector<SpoolInfo>& spools) {
                listed = std::any_of(spools.begin(), spools.end(),
                                     [active](const SpoolInfo& s) { return s.id == active; });
            },
            [](const MoonrakerError&) {});
        REQUIRE(active > 0);
        REQUIRE(listed);
    }

    SECTION("Spools have valid data") {
        api.spoolman().get_spoolman_spools(
            [&](const std::vector<SpoolInfo>& spools) {
                for (const auto& spool : spools) {
                    // Each spool should have basic info
                    REQUIRE(spool.id > 0);
                    REQUIRE(!spool.vendor.empty());
                    REQUIRE(!spool.material.empty());
                    REQUIRE(spool.initial_weight_g > 0);
                    REQUIRE(spool.remaining_weight_g >= 0);
                    REQUIRE(spool.remaining_weight_g <= spool.initial_weight_g);
                }
            },
            [](const MoonrakerError&) {});
    }

    SECTION("Has diverse materials") {
        std::set<std::string> materials;
        api.spoolman().get_spoolman_spools(
            [&](const std::vector<SpoolInfo>& spools) {
                for (const auto& spool : spools) {
                    materials.insert(spool.material);
                }
            },
            [](const MoonrakerError&) {});

        // Should have at least 5 different materials
        REQUIRE(materials.size() >= 5);
    }
}

TEST_CASE("MoonrakerAPIMock - set_active_spool", "[filament][mock]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SECTION("Changes active spool") {
        bool success_called = false;
        api.spoolman().set_active_spool(
            5, [&]() { success_called = true; },
            [](const MoonrakerError&) { FAIL("Error should not be called"); });

        REQUIRE(success_called);

        // Verify the change via get_spoolman_status
        api.spoolman().get_spoolman_status(
            [](bool /*connected*/, int active_spool_id) { REQUIRE(active_spool_id == 5); },
            [](const MoonrakerError&) {});
    }

    SECTION("Setting non-existent spool ID still succeeds") {
        // Mock doesn't validate IDs - that's the server's job
        bool success_called = false;
        api.spoolman().set_active_spool(
            9999, [&]() { success_called = true; }, [](const MoonrakerError&) {});

        REQUIRE(success_called);
    }
}

// ============================================================================
// MoonrakerAPIMock - Spoolman CRUD Tests
// ============================================================================

TEST_CASE("MoonrakerAPIMock - get_spoolman_vendors", "[filament][mock]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SECTION("Returns vendor list derived from spools") {
        bool callback_called = false;
        api.spoolman().get_spoolman_vendors(
            [&](const std::vector<VendorInfo>& vendors) {
                callback_called = true;
                // Should have multiple unique vendors from mock spools
                REQUIRE(vendors.size() > 0);
                // Each vendor should have a valid name
                for (const auto& v : vendors) {
                    REQUIRE(v.id > 0);
                    REQUIRE(!v.name.empty());
                }
            },
            [](const MoonrakerError&) { FAIL("Error callback should not be called"); });

        REQUIRE(callback_called);
    }

    SECTION("Vendors are deduplicated") {
        std::set<std::string> vendor_names;
        api.spoolman().get_spoolman_vendors(
            [&](const std::vector<VendorInfo>& vendors) {
                for (const auto& v : vendors) {
                    REQUIRE(vendor_names.find(v.name) == vendor_names.end());
                    vendor_names.insert(v.name);
                }
            },
            [](const MoonrakerError&) {});
    }
}

TEST_CASE("MoonrakerAPIMock - get_spoolman_filaments", "[filament][mock]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SECTION("Returns filament list") {
        bool callback_called = false;
        api.spoolman().get_spoolman_filaments(
            [&](const std::vector<FilamentInfo>& filaments) {
                callback_called = true;
                REQUIRE(filaments.size() > 0);
                for (const auto& f : filaments) {
                    REQUIRE(f.id > 0);
                    REQUIRE(!f.material.empty());
                }
            },
            [](const MoonrakerError&) { FAIL("Error callback should not be called"); });

        REQUIRE(callback_called);
    }
}

TEST_CASE("MoonrakerAPIMock - create_spoolman_vendor", "[filament][mock]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SECTION("Creates vendor and returns it") {
        nlohmann::json data;
        data["name"] = "Test Vendor";

        bool callback_called = false;
        api.spoolman().create_spoolman_vendor(
            data,
            [&](const VendorInfo& vendor) {
                callback_called = true;
                REQUIRE(vendor.id > 0);
                REQUIRE(vendor.name == "Test Vendor");
            },
            [](const MoonrakerError&) { FAIL("Error callback should not be called"); });

        REQUIRE(callback_called);
    }
}

TEST_CASE("MoonrakerAPIMock - create_spoolman_filament", "[filament][mock]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SECTION("Creates filament and returns it") {
        nlohmann::json data;
        data["material"] = "PETG";
        data["name"] = "Ocean Blue PETG";
        data["color_name"] = "Ocean Blue";
        data["color_hex"] = "#0077B6";
        data["density"] = 1.27f;
        data["diameter"] = 1.75f;
        data["weight"] = 1000.0f;

        bool callback_called = false;
        api.spoolman().create_spoolman_filament(
            data,
            [&](const FilamentInfo& filament) {
                callback_called = true;
                REQUIRE(filament.id > 0);
                REQUIRE(filament.material == "PETG");
                REQUIRE(filament.filament_name == "Ocean Blue PETG");
                REQUIRE(filament.color_hex == "#0077B6");
            },
            [](const MoonrakerError&) { FAIL("Error callback should not be called"); });

        REQUIRE(callback_called);
    }
}

TEST_CASE("MoonrakerAPIMock - create_spoolman_spool", "[filament][mock]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SECTION("Creates spool and adds to list") {
        size_t initial_count = 0;
        api.spoolman().get_spoolman_spools(
            [&](const std::vector<SpoolInfo>& spools) { initial_count = spools.size(); },
            [](const MoonrakerError&) {});

        nlohmann::json data;
        data["filament_id"] = 1;
        data["initial_weight"] = 800.0;
        data["spool_weight"] = 200.0;

        bool callback_called = false;
        api.spoolman().create_spoolman_spool(
            data,
            [&](const SpoolInfo& spool) {
                callback_called = true;
                REQUIRE(spool.id > 0);
                REQUIRE(spool.initial_weight_g == Catch::Approx(800.0));
                REQUIRE(spool.spool_weight_g == Catch::Approx(200.0));
            },
            [](const MoonrakerError&) { FAIL("Error callback should not be called"); });

        REQUIRE(callback_called);

        // Verify spool count increased
        api.spoolman().get_spoolman_spools(
            [&](const std::vector<SpoolInfo>& spools) {
                REQUIRE(spools.size() == initial_count + 1);
            },
            [](const MoonrakerError&) {});
    }
}

TEST_CASE("MoonrakerAPIMock - delete_spoolman_spool", "[filament][mock]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SECTION("Deletes spool from list") {
        size_t initial_count = 0;
        api.spoolman().get_spoolman_spools(
            [&](const std::vector<SpoolInfo>& spools) { initial_count = spools.size(); },
            [](const MoonrakerError&) {});

        REQUIRE(initial_count > 0);

        // Delete spool with ID 1
        bool callback_called = false;
        api.spoolman().delete_spoolman_spool(
            1, [&]() { callback_called = true; },
            [](const MoonrakerError&) { FAIL("Error callback should not be called"); });

        REQUIRE(callback_called);

        // Verify spool count decreased
        api.spoolman().get_spoolman_spools(
            [&](const std::vector<SpoolInfo>& spools) {
                REQUIRE(spools.size() == initial_count - 1);
                // Verify spool 1 is gone
                for (const auto& s : spools) {
                    REQUIRE(s.id != 1);
                }
            },
            [](const MoonrakerError&) {});
    }

    SECTION("Deleting a non-existent spool answers Spoolman's 404, as Moonraker relays it") {
        MoonrakerError got;
        api.spoolman().delete_spoolman_spool(
            9999, []() { FAIL("success should not be called"); },
            [&](const MoonrakerError& err) { got = err; });

        CHECK(got.code == -32601);
        CHECK(got.message == "Not Found");
        CHECK(got.is_not_found());
    }
}

TEST_CASE("MoonrakerAPIMock - update_spoolman_spool", "[filament][mock]") {
    MoonrakerClientMock client;
    PrinterState state;
    MoonrakerAPIMock api(client, state);

    SECTION("Updates remaining_weight field") {
        // Get initial weight of first spool
        double initial_weight = 0;
        api.spoolman().get_spoolman_spools(
            [&initial_weight](const std::vector<SpoolInfo>& spools) {
                REQUIRE(!spools.empty());
                initial_weight = spools[0].remaining_weight_g;
            },
            [](const MoonrakerError&) { FAIL("Failed to get spools"); });

        // Update the spool
        nlohmann::json patch;
        patch["remaining_weight"] = 42.0;

        bool callback_called = false;
        int spool_id = 1; // First mock spool
        api.spoolman().update_spoolman_spool(
            spool_id, patch, [&callback_called]() { callback_called = true; },
            [](const MoonrakerError&) { FAIL("Update should not fail"); });

        REQUIRE(callback_called);

        // Verify the weight was updated
        api.spoolman().get_spoolman_spools(
            [spool_id](const std::vector<SpoolInfo>& spools) {
                for (const auto& s : spools) {
                    if (s.id == spool_id) {
                        REQUIRE(s.remaining_weight_g == Catch::Approx(42.0));
                        return;
                    }
                }
                FAIL("Spool not found after update");
            },
            [](const MoonrakerError&) { FAIL("Failed to get spools after update"); });
    }
}

TEST_CASE("MoonrakerAPIMock - Spoolman-gated methods fail when disabled", "[filament][mock]") {
    // The real proxy errors when Spoolman is not connected, and the slot
    // editor's managed-state gating depends on these calls failing rather than
    // succeeding with mock data.
    MoonrakerClientMock client;
    PrinterState state;
    MoonrakerAPIMock api(client, state);
    client.set_mock_spoolman_enabled(false);
    auto& sm = api.spoolman();
    const int active_before = client.spoolman_mock().get_mock_active_spool_id();

    using Ok = std::function<void()>;
    using Err = std::function<void(const MoonrakerError&)>;
    const nlohmann::json weight = {{"remaining_weight", 1.0}};
    const nlohmann::json color = {{"color_hex", "00FF00"}};
    const nlohmann::json vendor = {{"name", "Acme"}};
    const nlohmann::json material = {{"material", "PLA"}};
    const nlohmann::json on_filament = {{"filament_id", 1}};
    const std::pair<const char*, std::function<void(Ok, Err)>> calls[] = {
        {"get_spoolman_spool",
         [&](Ok ok, Err e) { sm.get_spoolman_spool(1, [ok](const auto&) { ok(); }, e); }},
        {"get_spoolman_spools",
         [&](Ok ok, Err e) { sm.get_spoolman_spools([ok](const auto&) { ok(); }, e); }},
        {"update_spoolman_spool",
         [&](Ok ok, Err e) { sm.update_spoolman_spool(1, weight, ok, e); }},
        {"update_spoolman_filament",
         [&](Ok ok, Err e) { sm.update_spoolman_filament(300, color, ok, e); }},
        {"set_active_spool", [&](Ok ok, Err e) { sm.set_active_spool(9, ok, e); }},
        {"update_spoolman_spool_weight",
         [&](Ok ok, Err e) { sm.update_spoolman_spool_weight(1, 500.0, ok, e); }},
        {"update_spoolman_filament_color",
         [&](Ok ok, Err e) { sm.update_spoolman_filament_color(300, "00FF00", ok, e); }},
        {"get_spoolman_vendors",
         [&](Ok ok, Err e) { sm.get_spoolman_vendors([ok](const auto&) { ok(); }, e); }},
        {"get_spoolman_filaments",
         [&](Ok ok, Err e) { sm.get_spoolman_filaments([ok](const auto&) { ok(); }, e); }},
        {"get_spoolman_filaments(vendor_id)",
         [&](Ok ok, Err e) { sm.get_spoolman_filaments(2, [ok](const auto&) { ok(); }, e); }},
        {"create_spoolman_vendor",
         [&](Ok ok, Err e) { sm.create_spoolman_vendor(vendor, [ok](const auto&) { ok(); }, e); }},
        {"create_spoolman_filament",
         [&](Ok ok, Err e) {
             sm.create_spoolman_filament(material, [ok](const auto&) { ok(); }, e);
         }},
        {"create_spoolman_spool",
         [&](Ok ok, Err e) {
             sm.create_spoolman_spool(on_filament, [ok](const auto&) { ok(); }, e);
         }},
        {"delete_spoolman_spool", [&](Ok ok, Err e) { sm.delete_spoolman_spool(1, ok, e); }},
        {"delete_spoolman_vendor", [&](Ok ok, Err e) { sm.delete_spoolman_vendor(1, ok, e); }},
        {"delete_spoolman_filament", [&](Ok ok, Err e) { sm.delete_spoolman_filament(1, ok, e); }},
    };
    for (const auto& [name, call] : calls) {
        INFO(name);
        bool succeeded = false;
        std::optional<MoonrakerError> error;
        call([&] { succeeded = true; }, [&](const MoonrakerError& err) { error = err; });
        CHECK_FALSE(succeeded);
        REQUIRE(error.has_value());
        CHECK(error->code == -32601);
        CHECK_FALSE(error->is_not_found());
    }
    CHECK(client.spoolman_mock().get_mock_active_spool_id() == active_before);
    CHECK(client.spoolman_mock().created_vendors.empty());
    CHECK(client.spoolman_mock().created_filaments.empty());
    CHECK(client.spoolman_mock().created_spools.empty());
}

// ============================================================================
// JSON Null Handling Tests (server.spoolman.status parsing)
// ============================================================================

TEST_CASE("Spoolman status - spool_id null handling", "[filament][parsing]") {
    // This test validates parsing of server.spoolman.status responses.
    // When no spool is active, Moonraker returns: {"spool_id": null}
    // Must use null-safe pattern: check contains() && !is_null() before get<int>()
    // Using json::value() with null throws type_error.302.

    SECTION("null spool_id should return default value (0)") {
        // Simulate Moonraker response when no spool is active
        auto response = nlohmann::json::parse(R"({
            "result": {
                "spoolman_connected": true,
                "spool_id": null
            }
        })");

        const auto& result = response["result"];
        bool connected = result.value("spoolman_connected", false);

        // Use null-safe pattern (matches moonraker_api_advanced.cpp:1195)
        int active_spool_id = 0;
        if (result.contains("spool_id") && !result["spool_id"].is_null()) {
            active_spool_id = result["spool_id"].get<int>();
        }

        REQUIRE(connected == true);
        REQUIRE(active_spool_id == 0); // null should fall back to default 0
    }

    SECTION("integer spool_id still works normally") {
        auto response = nlohmann::json::parse(R"({
            "result": {
                "spoolman_connected": true,
                "spool_id": 42
            }
        })");

        const auto& result = response["result"];
        int active_spool_id = result.value("spool_id", 0);

        REQUIRE(active_spool_id == 42);
    }

    SECTION("missing spool_id uses default") {
        auto response = nlohmann::json::parse(R"({
            "result": {
                "spoolman_connected": true
            }
        })");

        const auto& result = response["result"];
        int active_spool_id = result.value("spool_id", 0);

        REQUIRE(active_spool_id == 0);
    }
}

// ============================================================================
// parse_spool_info — null numeric field tolerance (#1087)
//
// Spoolman serializes optional filament fields (settings_extruder_temp,
// settings_bed_temp) as present-but-null. A raw json::value("k", def) calls
// get<int>() on the null and throws type_error.302, which aborted the whole
// spool-list parse and left the "Choose Spool" picker stuck loading forever.
// These exercise the REAL parser so a regression to .value() fails the build.
// ============================================================================

TEST_CASE("parse_spool_info - null recommended temps do not throw (#1087)",
          "[filament][parsing][spoolman]") {
    using helix::spoolman_detail::parse_spool_info;

    SECTION("both settings temps null falls back to 0") {
        auto j = nlohmann::json::parse(R"({
            "id": 7,
            "remaining_weight": 800.0,
            "filament": {
                "id": 3,
                "material": "PLA",
                "name": "Jet Black",
                "settings_extruder_temp": null,
                "settings_bed_temp": null
            }
        })");

        SpoolInfo info;
        REQUIRE_NOTHROW(info = parse_spool_info(j));
        REQUIRE(info.id == 7);
        REQUIRE(info.material == "PLA");
        REQUIRE(info.nozzle_temp_recommended == 0);
        REQUIRE(info.bed_temp_recommended == 0);
    }

    SECTION("null top-level id and nested filament/vendor ids do not throw") {
        auto j = nlohmann::json::parse(R"({
            "id": null,
            "remaining_weight": 500.0,
            "filament": {
                "id": null,
                "material": "PETG",
                "vendor": {"id": null, "name": "eSUN"}
            }
        })");

        SpoolInfo info;
        REQUIRE_NOTHROW(info = parse_spool_info(j));
        REQUIRE(info.id == 0);
        REQUIRE(info.filament_id == 0);
        REQUIRE(info.vendor_id == 0);
        REQUIRE(info.vendor == "eSUN");
    }

    SECTION("present integer temps still parse correctly") {
        auto j = nlohmann::json::parse(R"({
            "id": 12,
            "filament": {
                "settings_extruder_temp": 215,
                "settings_bed_temp": 60
            }
        })");

        auto info = parse_spool_info(j);
        REQUIRE(info.nozzle_temp_recommended == 215);
        REQUIRE(info.bed_temp_recommended == 60);
    }
}

// ============================================================================
// parse_spool_info — nozzle/bed temperature
//
// Spoolman's filament carries ONE integer per heater, settings_extruder_temp
// and settings_bed_temp. apply_spool_to_slot() copies spool.nozzle_temp_min/max
// onto the slot, so the single value has to fill both ends or every
// Spoolman-sourced slot reads a 0/0 nozzle range.
// ============================================================================

TEST_CASE("parse_spool_info reads Spoolman's single nozzle and bed temperature",
          "[filament][parsing][spoolman]") {
    using helix::spoolman_detail::parse_spool_info;

    SECTION("the one value is the recommended value and both ends of the range") {
        auto j = nlohmann::json::parse(R"({
            "id": 21,
            "filament": {
                "id": 4,
                "material": "PETG",
                "settings_extruder_temp": 240,
                "settings_bed_temp": 80
            }
        })");

        auto info = parse_spool_info(j);
        CHECK(info.nozzle_temp_recommended == 240);
        CHECK(info.nozzle_temp_min == 240);
        CHECK(info.nozzle_temp_max == 240);
        CHECK(info.bed_temp_recommended == 80);
        CHECK(info.bed_temp_min == 80);
        CHECK(info.bed_temp_max == 80);
    }

    SECTION("present-but-null temperatures do not throw and read as 0") {
        // Spoolman serializes every optional numeric as null rather than
        // omitting it — a raw .value() on these throws type_error.302 and
        // aborts the whole spool-list parse (#1087).
        auto j = nlohmann::json::parse(R"({
            "id": 22,
            "filament": {
                "material": "PLA",
                "settings_extruder_temp": null,
                "settings_bed_temp": null
            }
        })");

        SpoolInfo info;
        REQUIRE_NOTHROW(info = parse_spool_info(j));
        CHECK(info.nozzle_temp_min == 0);
        CHECK(info.nozzle_temp_max == 0);
        CHECK(info.bed_temp_min == 0);
        CHECK(info.bed_temp_max == 0);
    }

    SECTION("the temperature reaches the slot through apply_spool_to_slot") {
        auto j = nlohmann::json::parse(R"({
            "id": 23,
            "filament": {
                "material": "PETG",
                "settings_extruder_temp": 240,
                "settings_bed_temp": 80
            }
        })");

        SlotInfo slot;
        apply_spool_to_slot(slot, parse_spool_info(j));
        CHECK(slot.nozzle_temp_min == 240);
        CHECK(slot.nozzle_temp_max == 240);
        CHECK(slot.bed_temp == 80);
    }
}

TEST_CASE("get_spoolman_filaments reads Spoolman's single temperature onto the range",
          "[filament][parsing][spoolman]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    client.spoolman_mock().get_mock_spools().clear();
    client.spoolman_mock().add_vendor(1, "Polymaker");
    client.spoolman_mock().add_filament(100, 1, "PETG", "FF0000");

    // The server stores one value; seed it through the PATCH Spoolman accepts.
    bool patched = false;
    api.spoolman().update_spoolman_filament(
        100, {{"settings_extruder_temp", 240}, {"settings_bed_temp", 80}},
        [&]() { patched = true; }, [](const MoonrakerError&) {});
    REQUIRE(patched);

    std::vector<FilamentInfo> got;
    api.spoolman().get_spoolman_filaments([&](const std::vector<FilamentInfo>& f) { got = f; },
                                          [](const MoonrakerError&) {});
    REQUIRE_FALSE(got.empty());
    CHECK(got[0].id == 100);
    CHECK(got[0].nozzle_temp_min == 240);
    CHECK(got[0].nozzle_temp_max == 240);
    CHECK(got[0].bed_temp_min == 80);
    CHECK(got[0].bed_temp_max == 80);
}

TEST_CASE("mock Spoolman rejects a non-integer filament temperature like Spoolman",
          "[filament][spoolman][mock]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    int code = 0;
    api.spoolman().create_spoolman_filament(
        {{"vendor_id", 1},
         {"material", "PLA"},
         {"density", 1.24},
         {"diameter", 1.75},
         {"settings_extruder_temp", {{"min", 190}, {"max", 220}}}},
        [](const FilamentInfo&) { FAIL("an object temperature must not be accepted"); },
        [&](const MoonrakerError& err) { code = err.code; });
    CHECK(code == 422);
}

// ============================================================================
// apply_spool_to_slot — what it puts in spool_name
//
// spool_name means "a filament name" on every other writer: AFC parses it from
// filament_name, CFS from the flat schema's `name`, Snapmaker from the RFID
// SUB_TYPE. docs/specs/filament_slots.md requires the lane_data field to be
// "distinct from vendor + material", and OrcaSlicer / Happy Hare read it under
// that meaning. Spoolman's real filament name arrives in SpoolInfo::color_name
// — parse_spool_info() maps filament.name there, so the field is misnamed
// rather than miscarrying.
// ============================================================================

TEST_CASE("apply_spool_to_slot hands the slot the real filament name",
          "[filament][spoolman][regression]") {
    using helix::spoolman_detail::parse_spool_info;

    auto j = nlohmann::json::parse(R"({
        "id": 42,
        "filament": {
            "id": 12,
            "name": "Ambrosia Pink",
            "material": "PLA",
            "multi_color_hexes": "#D4AF37,#C0C0C0",
            "vendor": {"id": 4, "name": "Polymaker"}
        }
    })");

    const SpoolInfo spool = parse_spool_info(j);
    REQUIRE(spool.filament_name == "Ambrosia Pink"); // filament.name lands here
    REQUIRE(spool.vendor == "Polymaker");

    SlotInfo slot;
    apply_spool_to_slot(slot, spool);

    CHECK(slot.spool_name == "Ambrosia Pink");
    // The synthesis this replaced. "Polymaker PLA" is not a name: it repeats
    // brand and material, which compose_filament_label() then dedups away, so
    // the colour identity of the spool never reaches the card.
    CHECK(slot.spool_name != "Polymaker PLA");
    CHECK(slot.brand == "Polymaker");
    CHECK(slot.material == "PLA");
    CHECK(slot.spoolman_id == 42);

    SECTION("multi-colour hexes reach the slot rather than being dropped") {
        CHECK(slot.multi_color_hexes == "#D4AF37,#C0C0C0");
    }

    SECTION("a spool with no filament name leaves the name blank, not fabricated") {
        // Blank is the unset sentinel the slot/override merge policy expects.
        // Fabricating "eSUN PETG" here would make the field indistinguishable
        // from a user-entered label on the wire.
        SpoolInfo bare;
        bare.id = 7;
        bare.vendor = "eSUN";
        bare.material = "PETG";

        SlotInfo bare_slot;
        apply_spool_to_slot(bare_slot, bare);

        CHECK(bare_slot.spool_name.empty());
        CHECK(bare_slot.brand == "eSUN");
        CHECK(bare_slot.material == "PETG");
    }
}

// ============================================================================
// filter_spools Tests
// ============================================================================

static std::vector<SpoolInfo> make_filter_test_spools() {
    std::vector<SpoolInfo> spools;

    SpoolInfo s1;
    s1.id = 1;
    s1.vendor = "Polymaker";
    s1.material = "PLA";
    s1.filament_name = "Jet Black";
    s1.location = "Shelf A";
    spools.push_back(s1);

    SpoolInfo s2;
    s2.id = 2;
    s2.vendor = "eSUN";
    s2.material = "PETG";
    s2.filament_name = "Blue";
    spools.push_back(s2);

    SpoolInfo s3;
    s3.id = 3;
    s3.vendor = "Polymaker";
    s3.material = "ASA";
    s3.filament_name = "Red";
    s3.location = "Shelf A";
    spools.push_back(s3);

    SpoolInfo s4;
    s4.id = 42;
    s4.vendor = "Hatchbox";
    s4.material = "PLA";
    s4.filament_name = "White";
    spools.push_back(s4);

    return spools;
}

// Spools: #1 Polymaker PLA Jet Black (Shelf A), #2 eSUN PETG Blue, #3 Polymaker
// ASA Red (Shelf A), #42 Hatchbox PLA White. Every term must match (AND), case
// is ignored, and a term matches by substring or, failing that, within an edit
// distance that tightens to one edit for three-character terms.
TEST_CASE("filter_spools matches every term against id, vendor, material, name and location",
          "[filament][filter]") {
    const auto spools = make_filter_test_spools();
    struct Row {
        const char* query;
        std::vector<int> ids;
    };
    const Row rows[] = {
        {"", {1, 2, 3, 42}},
        {"   ", {1, 2, 3, 42}},
        {"PLA", {1, 42}},
        {"polymaker", {1, 3}},
        {"polymaker pla", {1}},
        {"ESUN petg", {2}},
        {"#42", {42}},
        {"42", {42}},
        {"blue", {2}},
        {"nonexistent", {}},
        {"shelf", {1, 3}},
        {"shelf pla", {1}},
        {"hatchbox", {42}}, // a spool with no location still matches
        {"poly", {1, 3}},
        // Typos: a dropped letter, a wrong letter, a transposition.
        {"polmaker", {1, 3}},
        {"peyg", {2}},
        {"htachbox", {42}},
        {"polmaker pla", {1}},
        {"pls", {1, 42}},
        {"xya", {}},
        {"xyz", {}},
    };
    for (const auto& row : rows) {
        INFO("query \"" << row.query << "\"");
        std::vector<int> got;
        for (const auto& s : filter_spools(spools, row.query))
            got.push_back(s.id);
        CHECK(got == row.ids);
    }
    CHECK(filter_spools(std::vector<SpoolInfo>{}, "PLA").empty());
}

TEST_CASE("build_searchable_text - id vendor material name location, lowercased",
          "[filament][filter]") {
    SpoolInfo s;
    s.id = 42;
    s.vendor = "Hatchbox";
    s.material = "PLA";
    s.filament_name = "White";
    s.location = "Shelf A";
    CHECK(build_searchable_text(s) == "#42 hatchbox pla white shelf a");
}

TEST_CASE("filter_spools - prebuilt searchables match inline results", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    std::vector<std::string> searchables;
    searchables.reserve(spools.size());
    for (const auto& s : spools) {
        searchables.push_back(build_searchable_text(s));
    }

    // Covers substring, fuzzy-typo, ID, multi-term AND and no-match paths.
    for (const char* q : {"", "  ", "poly", "polymeker", "#42", "42", "hatch", "polymaker shelf",
                          "PLA", "zzz nothing"}) {
        auto via_prebuilt = filter_spools(spools, q, searchables);
        auto inline_built = filter_spools(spools, q);
        INFO("query: " << q);
        REQUIRE(via_prebuilt.size() == inline_built.size());
        for (size_t i = 0; i < via_prebuilt.size(); ++i) {
            REQUIRE(via_prebuilt[i].id == inline_built[i].id);
        }
    }
}

TEST_CASE("filter_spools - stale searchables fall back to inline matching", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    // Wrong-size cache (inventory changed after precompute): must still
    // filter correctly by building the strings inline, not mis-filter.
    std::vector<std::string> stale(2, "#1 whatever");
    auto via_stale = filter_spools(spools, "poly", stale);
    auto inline_built = filter_spools(spools, "poly");
    REQUIRE(via_stale.size() == inline_built.size());
    for (size_t i = 0; i < via_stale.size(); ++i) {
        REQUIRE(via_stale[i].id == inline_built[i].id);
    }
}

TEST_CASE("SpoolInfo - location field parsed from JSON", "[filament][parsing]") {
    // This test uses the mock API which internally calls parse_spool_info()
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Create a spool with location set
    nlohmann::json body;
    body["filament_id"] = 1;
    body["location"] = "Shelf B";
    std::string created_location;

    api.spoolman().create_spoolman_spool(
        body, [&](const SpoolInfo& spool) { created_location = spool.location; },
        [](const MoonrakerError&) { FAIL("create failed"); });

    REQUIRE(created_location == "Shelf B");
}

TEST_CASE("SpoolInfo - location defaults to empty when null in JSON", "[filament][parsing]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Create a spool without location — safe_string returns "" for missing keys
    nlohmann::json body;
    body["filament_id"] = 1;
    std::string created_location = "should-be-cleared";

    api.spoolman().create_spoolman_spool(
        body, [&](const SpoolInfo& spool) { created_location = spool.location; },
        [](const MoonrakerError&) { FAIL("create failed"); });

    REQUIRE(created_location.empty());
}

// ============================================================================
// MoonrakerAPIMock - Filament Persistence & Patching Tests
// ============================================================================

TEST_CASE("Mock persists created filaments", "[spoolman][mock]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Create a filament
    nlohmann::json filament_data;
    filament_data["material"] = "PETG";
    filament_data["name"] = "Blue PETG";
    filament_data["color_name"] = "Blue";
    filament_data["color_hex"] = "#0000FF";
    filament_data["vendor_id"] = 1;
    filament_data["density"] = 1.27;
    filament_data["diameter"] = 1.75;

    FilamentInfo created;
    api.spoolman().create_spoolman_filament(
        filament_data, [&](const FilamentInfo& f) { created = f; }, nullptr);
    REQUIRE(created.id > 0);

    // Verify it appears in subsequent filament list
    std::vector<FilamentInfo> filaments;
    api.spoolman().get_spoolman_filaments(
        [&](const std::vector<FilamentInfo>& list) { filaments = list; }, nullptr);

    bool found = false;
    for (const auto& f : filaments) {
        if (f.id == created.id) {
            found = true;
            REQUIRE(f.material == "PETG");
            REQUIRE(f.filament_name == "Blue PETG");
        }
    }
    REQUIRE(found);
}

TEST_CASE("Mock update_spoolman_spool supports filament_id patch", "[spoolman][mock]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    auto& spools = client.spoolman_mock().get_mock_spools();
    int spool_id = spools[0].id;
    int original_filament_id = spools[0].filament_id;

    nlohmann::json patch;
    patch["filament_id"] = 999;

    bool success = false;
    api.spoolman().update_spoolman_spool(spool_id, patch, [&]() { success = true; }, nullptr);

    REQUIRE(success);
    REQUIRE(spools[0].filament_id == 999);
    REQUIRE(spools[0].filament_id != original_filament_id);
}

// ============================================================================
// Spoolman Filament Creation Data Validation
// These tests verify we send the required fields Spoolman expects.
// Missing density/diameter causes 422 Unprocessable Entity.
// ============================================================================

TEST_CASE("Filament creation data includes required Spoolman fields", "[spoolman][api]") {
    // Reproduce the data construction from SpoolWizardOverlay::create_filament_then_spool()
    // to verify density and diameter are always present.

    struct TestFilament {
        std::string material;
        std::string color_hex;
        std::string color_name;
        double density = 0;
        double diameter = 1.75;
        double weight = 0;
        double spool_weight = 0;
    };

    auto build_filament_data = [](const TestFilament& fil, int vendor_id) -> nlohmann::json {
        nlohmann::json data;
        data["vendor_id"] = vendor_id;
        data["name"] = fil.material + " " + fil.color_name;
        data["material"] = fil.material;
        if (!fil.color_hex.empty()) {
            data["color_hex"] = fil.color_hex;
        }
        // density and diameter are REQUIRED by Spoolman (no defaults in their API)
        data["density"] = fil.density > 0 ? fil.density : 1.24;
        data["diameter"] = fil.diameter > 0 ? fil.diameter : 1.75;
        if (fil.weight > 0) {
            data["weight"] = fil.weight;
        }
        if (fil.spool_weight > 0) {
            data["spool_weight"] = fil.spool_weight;
        }
        return data;
    };

    SECTION("Filament with all data populated") {
        TestFilament fil{.material = "PLA",
                         .color_hex = "1A1A2E",
                         .color_name = "Jet Black",
                         .density = 1.24,
                         .diameter = 1.75,
                         .weight = 1000};

        auto data = build_filament_data(fil, 1);
        REQUIRE(data.contains("density"));
        REQUIRE(data.contains("diameter"));
        REQUIRE(data["density"].get<double>() == Catch::Approx(1.24));
        REQUIRE(data["diameter"].get<double>() == Catch::Approx(1.75));
    }

    SECTION("Filament with zero density gets default 1.24") {
        TestFilament fil{.material = "PLA", .density = 0};

        auto data = build_filament_data(fil, 1);
        REQUIRE(data.contains("density"));
        REQUIRE(data["density"].get<double>() == Catch::Approx(1.24));
    }

    SECTION("Filament with zero diameter gets default 1.75") {
        TestFilament fil{.material = "PLA", .diameter = 0};

        auto data = build_filament_data(fil, 1);
        REQUIRE(data.contains("diameter"));
        REQUIRE(data["diameter"].get<double>() == Catch::Approx(1.75));
    }

    SECTION("Filament always has density and diameter — no conditionals") {
        TestFilament fil{.material = "PETG"};

        auto data = build_filament_data(fil, 5);
        // These must ALWAYS be present — Spoolman returns 422 without them
        REQUIRE(data.contains("density"));
        REQUIRE(data.contains("diameter"));
        REQUIRE(data["density"].get<double>() > 0);
        REQUIRE(data["diameter"].get<double>() > 0);
        // Optional fields should NOT be present when zero
        REQUIRE_FALSE(data.contains("weight"));
        REQUIRE_FALSE(data.contains("spool_weight"));
    }

    SECTION("2.85mm filament diameter is preserved") {
        TestFilament fil{.material = "PLA", .density = 1.24, .diameter = 2.85};

        auto data = build_filament_data(fil, 1);
        REQUIRE(data["diameter"].get<double>() == Catch::Approx(2.85));
    }

    SECTION("Custom density from material DB is preserved") {
        TestFilament fil{.material = "TPU", .density = 1.21};

        auto data = build_filament_data(fil, 3);
        REQUIRE(data["density"].get<double>() == Catch::Approx(1.21));
    }
}

TEST_CASE("FilamentInfo diameter populates FilamentEntry", "[spoolman]") {
    FilamentInfo fi;
    fi.material = "PETG";
    fi.density = 1.27f;
    fi.diameter = 2.85f;

    SpoolWizardOverlay::FilamentEntry entry;
    entry.density = fi.density;
    entry.diameter = fi.diameter;

    REQUIRE(entry.density == Catch::Approx(1.27));
    REQUIRE(entry.diameter == Catch::Approx(2.85));
}

// ============================================================================
// JSON Null-Safe Weight Parsing Tests
// ============================================================================

TEST_CASE("safe_double handles null initial_weight from Spoolman", "[filament][parsing]") {
    using helix::json_util::safe_double;

    SECTION("null initial_weight returns 0") {
        auto j = nlohmann::json::parse(R"({"remaining_weight": 800.0, "initial_weight": null})");
        REQUIRE(safe_double(j, "initial_weight") == 0.0);
        REQUIRE(safe_double(j, "remaining_weight") == Catch::Approx(800.0));
    }

    SECTION("missing initial_weight returns 0") {
        auto j = nlohmann::json::parse(R"({"remaining_weight": 800.0})");
        REQUIRE(safe_double(j, "initial_weight") == 0.0);
    }

    SECTION("present initial_weight parsed correctly") {
        auto j = nlohmann::json::parse(R"({"initial_weight": 1000.0})");
        REQUIRE(safe_double(j, "initial_weight") == Catch::Approx(1000.0));
    }
}

TEST_CASE("parse_spool_info fallback: filament.weight for initial_weight", "[filament][parsing]") {
    using helix::json_util::safe_double;

    SECTION("filament.weight used when initial_weight is null") {
        auto j = nlohmann::json::parse(R"({
            "remaining_weight": 800.0,
            "initial_weight": null,
            "filament": {"weight": 1000.0, "material": "PLA"}
        })");

        double initial = safe_double(j, "initial_weight");
        REQUIRE(initial == 0.0);

        // Simulate fallback to filament.weight
        if (initial <= 0 && j.contains("filament") && j["filament"].is_object()) {
            initial = safe_double(j["filament"], "weight");
        }
        REQUIRE(initial == Catch::Approx(1000.0));

        SpoolInfo spool;
        spool.initial_weight_g = initial;
        spool.remaining_weight_g = safe_double(j, "remaining_weight");
        REQUIRE(spool.remaining_percent() == Catch::Approx(80.0));
    }

    SECTION("used_weight fallback when both initial_weight and filament.weight are null") {
        auto j = nlohmann::json::parse(R"({
            "remaining_weight": 800.0,
            "initial_weight": null,
            "used_weight": 200.0,
            "filament": {"weight": null, "material": "PLA"}
        })");

        double initial = safe_double(j, "initial_weight");
        if (initial <= 0 && j.contains("filament") && j["filament"].is_object()) {
            initial = safe_double(j["filament"], "weight");
        }
        double used = safe_double(j, "used_weight");
        if (initial <= 0 && used > 0) {
            initial = safe_double(j, "remaining_weight") + used;
        }
        REQUIRE(initial == Catch::Approx(1000.0));

        SpoolInfo spool;
        spool.initial_weight_g = initial;
        spool.remaining_weight_g = safe_double(j, "remaining_weight");
        REQUIRE(spool.remaining_percent() == Catch::Approx(80.0));
    }

    SECTION("explicit initial_weight takes priority over fallbacks") {
        auto j = nlohmann::json::parse(R"({
            "remaining_weight": 800.0,
            "initial_weight": 1200.0,
            "used_weight": 200.0,
            "filament": {"weight": 1000.0}
        })");

        double initial = safe_double(j, "initial_weight");
        // Should NOT fall through to filament.weight since initial_weight > 0
        REQUIRE(initial == Catch::Approx(1200.0));
    }
}

TEST_CASE("SpoolInfo vendor_id populated from mock spool", "[filament][spoolman]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Use unique ID to avoid collision with other tests' mock data
    auto& spools = client.spoolman_mock().get_mock_spools();
    SpoolInfo test_spool;
    test_spool.id = 99901;
    test_spool.filament_id = 99910;
    test_spool.vendor = "TestVendor";
    test_spool.vendor_id = 77;
    test_spool.material = "PLA";
    test_spool.color_hex = "FF0000";
    spools.push_back(test_spool);

    bool fetched = false;
    api.spoolman().get_spoolman_spool(
        99901,
        [&](const std::optional<SpoolInfo>& spool) {
            REQUIRE(spool.has_value());
            REQUIRE(spool->vendor == "TestVendor");
            REQUIRE(spool->filament_id == 99910);
            REQUIRE(spool->vendor_id == 77);
            fetched = true;
        },
        [](const MoonrakerError&) {});
    REQUIRE(fetched);
}

// ============================================================================
// sort_spools_by_recency — picker ordering (#1071). Single descending key:
// max(last_used, registered). A never-used spool competes on its registration
// date rather than being banished below every used spool.
// ============================================================================

namespace {

/// Build a spool with explicit timestamps for ordering tests.
SpoolInfo dated_spool(int id, const std::string& last_used, const std::string& registered) {
    SpoolInfo s;
    s.id = id;
    s.last_used = last_used;
    s.registered = registered;
    return s;
}

std::vector<int> ids_of(const std::vector<SpoolInfo>& spools) {
    std::vector<int> ids;
    ids.reserve(spools.size());
    for (const auto& s : spools)
        ids.push_back(s.id);
    return ids;
}

} // namespace

TEST_CASE("sort_spools_by_recency puts the most recently used or registered spool first (#1071)",
          "[filament][spoolman][sort]") {
    struct Row {
        const char* name;
        std::vector<SpoolInfo> spools;
        std::vector<int> want;
    };
    const Row rows[] = {
        // A spool registered today and never used outranks one used yesterday.
        {"a newly added never-used spool",
         {dated_spool(11, "2026-07-18T09:00:00", "2026-01-02T00:00:00"),
          dated_spool(21, "", "2026-07-19T08:00:00")},
         {21, 11}},
        {"a used spool over an older-registered unused one",
         {dated_spool(30, "", "2026-07-15T00:00:00"),
          dated_spool(31, "2026-07-19T00:00:00", "2026-03-01T00:00:00")},
         {31, 30}},
        {"never-used spools by registration",
         {dated_spool(50, "", "2026-02-01T00:00:00"), dated_spool(51, "", "2026-08-01T00:00:00"),
          dated_spool(52, "", "2026-05-01T00:00:00")},
         {51, 52, 50}},
        {"an empty list", {}, {}},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        auto spools = row.spools;
        sort_spools_by_recency(spools);
        CHECK(ids_of(spools) == row.want);
    }
}

TEST_CASE("sort_spools_by_recency key is the max of last_used and registered",
          "[filament][spoolman][sort]") {
    // Neither field alone yields this order:
    //   by last_used only  -> {41, 40} (40 has none, sinks)
    //   by registered only -> {40, 41}
    //   by max(...)        -> {40, 41} via 40's registration beating 41's use
    SpoolInfo used_a_while_ago = dated_spool(41, "2026-04-01T00:00:00", "2025-01-01T00:00:00");
    SpoolInfo added_recently = dated_spool(40, "", "2026-05-01T00:00:00");

    CHECK(spool_recency_key(added_recently) > spool_recency_key(used_a_while_ago));

    // A spool whose registration post-dates its own last_used keys off registration.
    SpoolInfo re_registered = dated_spool(42, "2026-01-01T00:00:00", "2026-06-01T00:00:00");
    CHECK(spool_recency_key(re_registered) ==
          spool_recency_key(dated_spool(0, "", "2026-06-01T00:00:00")));

    // And one used after registration keys off last_used.
    SpoolInfo used_after = dated_spool(43, "2026-06-01T00:00:00", "2026-01-01T00:00:00");
    CHECK(spool_recency_key(used_after) ==
          spool_recency_key(dated_spool(0, "2026-06-01T00:00:00", "")));
}

TEST_CASE("sort_spools_by_recency sorts spools with no usable timestamp last",
          "[filament][spoolman][sort]") {
    std::vector<SpoolInfo> spools;
    spools.push_back(dated_spool(60, "", ""));                    // no timestamps at all
    spools.push_back(dated_spool(61, "", "not-a-timestamp"));     // unparseable
    spools.push_back(dated_spool(62, "", "2020-01-01T00:00:00")); // ancient but dated

    sort_spools_by_recency(spools);

    // The dated spool wins however old it is; undated ones fall to id tie-break.
    CHECK(spools[0].id == 62);
    CHECK(spool_recency_key(spools[1]) == kSpoolRecencyNone);
    CHECK(spool_recency_key(spools[2]) == kSpoolRecencyNone);
    CHECK(ids_of(spools) == std::vector<int>{62, 61, 60});
}

TEST_CASE("sort_spools_by_recency ordering is deterministic across refreshes",
          "[filament][spoolman][sort]") {
    // Equal keys must not churn between fetches, which arrive in arbitrary order.
    const std::string same = "2026-05-05T00:00:00";
    std::vector<SpoolInfo> a{dated_spool(1, "", same), dated_spool(2, "", same),
                             dated_spool(3, "", same)};
    std::vector<SpoolInfo> b{dated_spool(3, "", same), dated_spool(1, "", same),
                             dated_spool(2, "", same)};

    sort_spools_by_recency(a);
    sort_spools_by_recency(b);

    CHECK(ids_of(a) == std::vector<int>{3, 2, 1}); // higher id first
    CHECK(ids_of(a) == ids_of(b));                 // same result from a different input order
}

TEST_CASE("parse_spool_timestamp handles the timestamp shapes Spoolman emits",
          "[filament][spoolman][sort]") {
    // Naive and explicit-UTC forms of the same instant agree.
    const auto naive = parse_spool_timestamp("2026-07-19T12:34:56");
    const auto zulu = parse_spool_timestamp("2026-07-19T12:34:56Z");
    REQUIRE(naive.has_value());
    REQUIRE(zulu.has_value());
    CHECK(*naive == *zulu);

    // Fractional seconds are accepted and truncated.
    const auto frac = parse_spool_timestamp("2026-07-19T12:34:56.123456Z");
    REQUIRE(frac.has_value());
    CHECK(*frac == *zulu);

    // A +02:00 offset is two hours EARLIER in UTC than the same wall-clock in Z.
    const auto plus2 = parse_spool_timestamp("2026-07-19T12:34:56+02:00");
    REQUIRE(plus2.has_value());
    CHECK(*plus2 == *zulu - 2 * 3600);

    // This is exactly the case string comparison gets wrong: lexically
    // "...56+02:00" > "...56Z" is false, yet the +02:00 instant is earlier.
    // Parsing gives the correct relation regardless of suffix.
    const auto minus5 = parse_spool_timestamp("2026-07-19T12:34:56-05:00");
    REQUIRE(minus5.has_value());
    CHECK(*minus5 == *zulu + 5 * 3600);

    // Junk and empties yield nullopt rather than a bogus epoch.
    CHECK_FALSE(parse_spool_timestamp("").has_value());
    CHECK_FALSE(parse_spool_timestamp("nope").has_value());
    CHECK_FALSE(parse_spool_timestamp("2026-07-19").has_value()); // date only, too short
}

TEST_CASE("mock Spoolman has no external vendor route, as Spoolman has none", "[spoolman][mock]") {
    // Spoolman's external routes are /external/filament, /external/filament/search
    // and /external/material.
    MoonrakerClientMock client;
    MoonrakerError got;
    client.send_jsonrpc(
        "server.spoolman.proxy", {{"request_method", "GET"}, {"path", "/v1/external/vendor"}},
        [](const nlohmann::json&) { FAIL("Spoolman serves no external vendor list"); },
        [&](const MoonrakerError& err) { got = err; });
    CHECK(got.is_not_found());
}

TEST_CASE("mock Spoolman embeds a listed filament in a spool created on it", "[spoolman][mock]") {
    // Spoolman serves a spool with its whole filament, whichever filament it
    // was created on.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    std::vector<FilamentInfo> filaments;
    api.spoolman().get_spoolman_filaments(
        [&](const std::vector<FilamentInfo>& f) { filaments = f; }, nullptr);
    REQUIRE_FALSE(filaments.empty());
    const FilamentInfo& listed = filaments.front();

    SpoolInfo created;
    api.spoolman().create_spoolman_spool(
        {{"filament_id", listed.id}}, [&](const SpoolInfo& s) { created = s; }, nullptr);
    CHECK(created.material == listed.material);
    CHECK(created.vendor == listed.vendor_name);
    CHECK(created.nozzle_temp_recommended == listed.nozzle_temp_max);
}

// ============================================================================
// SpoolmanDB search
// ============================================================================

TEST_CASE("parse_external_filament reads Spoolman's ExternalFilament shape",
          "[spoolman][spoolman_db]") {
    using helix::spoolman_detail::parse_external_filament;

    SECTION("a single-colour entry") {
        auto f = parse_external_filament(nlohmann::json::parse(R"({
            "id": "polymaker_pla_polyliteplablack_1000_175_n", "manufacturer": "Polymaker",
            "name": "PolyLite PLA Black", "material": "PLA", "density": 1.24, "weight": 1000,
            "spool_weight": 140, "diameter": 1.75, "color_hex": "1A1A1A",
            "extruder_temp": 210, "bed_temp": 60, "translucent": false, "glow": false
        })"));
        REQUIRE(f.has_value());
        CHECK(f->id == "polymaker_pla_polyliteplablack_1000_175_n");
        CHECK(f->manufacturer == "Polymaker");
        CHECK(f->name == "PolyLite PLA Black");
        CHECK(f->material == "PLA");
        CHECK(f->color_hex == "1A1A1A");
        CHECK(f->color_hexes.empty());
        CHECK(f->density == Catch::Approx(1.24f));
        CHECK(f->diameter == Catch::Approx(1.75f));
        CHECK(f->weight == Catch::Approx(1000.0f));
        CHECK(f->spool_weight == Catch::Approx(140.0f));
        CHECK(f->extruder_temp == 210);
        CHECK(f->bed_temp == 60);
    }

    SECTION("a multi-colour entry with the optional fields omitted") {
        // Spoolman serves the catalog with response_model_exclude_none.
        auto f = parse_external_filament(nlohmann::json::parse(R"({
            "id": "esun_silkpla_rainbow", "manufacturer": "eSUN", "name": "Silk PLA Rainbow",
            "material": "PLA", "density": 1.24, "weight": 1000, "diameter": 1.75,
            "color_hexes": ["E53935", "FFEB3B"], "multi_color_direction": "coaxial",
            "translucent": false, "glow": false
        })"));
        REQUIRE(f.has_value());
        CHECK(f->color_hex.empty());
        CHECK(f->color_hexes == std::vector<std::string>{"E53935", "FFEB3B"});
        CHECK(f->multi_color_direction == "coaxial");
        CHECK(f->spool_weight == 0.0f);
        CHECK(f->extruder_temp == 0);
        CHECK(f->bed_temp == 0);
    }

    SECTION("malformed entries never throw") {
        CHECK_FALSE(parse_external_filament(nlohmann::json::array()).has_value());
        CHECK_FALSE(parse_external_filament(nlohmann::json("x")).has_value());
        CHECK_FALSE(parse_external_filament(nlohmann::json::object()).has_value());
        CHECK_FALSE(parse_external_filament({{"id", 12}, {"name", "numeric id"}}).has_value());

        std::optional<ExternalFilament> f;
        REQUIRE_NOTHROW(f = parse_external_filament({{"id", "odd"},
                                                     {"manufacturer", nullptr},
                                                     {"density", "heavy"},
                                                     {"extruder_temp", nullptr},
                                                     {"color_hexes", {"FF0000", 7, nullptr}}}));
        REQUIRE(f.has_value());
        CHECK(f->manufacturer.empty());
        CHECK(f->density == 0.0f);
        CHECK(f->extruder_temp == 0);
        CHECK(f->color_hexes == std::vector<std::string>{"FF0000"});
    }
}

TEST_CASE("search_spoolman_external_filaments searches the catalog through the proxy",
          "[spoolman][spoolman_db]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    auto search = [&](const std::string& query, int limit) {
        std::vector<ExternalFilament> got;
        bool ok = false;
        api.spoolman().search_spoolman_external_filaments(
            query, limit,
            [&](const std::vector<ExternalFilament>& r) {
                got = r;
                ok = true;
            },
            [](const MoonrakerError&) {});
        REQUIRE(ok);
        return got;
    };

    SECTION("every word must match, case-insensitively") {
        auto poly = search("poly", 25);
        REQUIRE_FALSE(poly.empty());
        for (const auto& f : poly) {
            CHECK(f.manufacturer == "Polymaker");
        }
        auto petg = search("POLYMAKER petg BLUE", 25);
        REQUIRE(petg.size() == 1);
        CHECK(petg[0].name == "PolyLite PETG Blue");
    }

    SECTION("the limit caps the results") {
        CHECK(search("pla", 2).size() == 2);
    }

    SECTION("an older server answers not-found") {
        client.spoolman_mock().set_external_search_supported(false);
        MoonrakerError got;
        api.spoolman().search_spoolman_external_filaments(
            "poly", 25, [](const std::vector<ExternalFilament>&) { FAIL("no route"); },
            [&](const MoonrakerError& err) { got = err; });
        CHECK(got.is_not_found());
    }
}

TEST_CASE("mock Spoolman names the vendor of a filament created on a listed vendor",
          "[spoolman][mock]") {
    // Spoolman serves a filament with its whole vendor, whichever vendor it was
    // created under.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    std::vector<VendorInfo> vendors;
    api.spoolman().get_spoolman_vendors([&](const std::vector<VendorInfo>& v) { vendors = v; },
                                        nullptr);
    REQUIRE_FALSE(vendors.empty());

    FilamentInfo created;
    api.spoolman().create_spoolman_filament(
        {{"vendor_id", vendors.front().id},
         {"material", "PLA"},
         {"density", 1.24},
         {"diameter", 1.75}},
        [&](const FilamentInfo& f) { created = f; }, nullptr);
    CHECK(created.vendor_name == vendors.front().name);
}

TEST_CASE("mock Spoolman serves only the asked vendor's filaments", "[spoolman][mock]") {
    // The spool wizard's filament step lists exactly what /v1/filament?vendor.id= returns.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    std::vector<VendorInfo> vendors;
    api.spoolman().get_spoolman_vendors([&](const std::vector<VendorInfo>& v) { vendors = v; },
                                        nullptr);
    std::vector<FilamentInfo> all;
    api.spoolman().get_spoolman_filaments([&](const std::vector<FilamentInfo>& f) { all = f; },
                                          nullptr);
    REQUIRE(vendors.size() > 1);

    size_t served = 0;
    for (const auto& vendor : vendors) {
        std::vector<FilamentInfo> got;
        api.spoolman().get_spoolman_filaments(
            vendor.id, [&](const std::vector<FilamentInfo>& f) { got = f; }, nullptr);
        for (const auto& f : got) {
            CHECK(f.vendor_id == vendor.id);
            CHECK(f.vendor_name == vendor.name);
        }
        served += got.size();
    }
    // Every listed filament belongs to exactly one vendor.
    CHECK(served == all.size());
}
