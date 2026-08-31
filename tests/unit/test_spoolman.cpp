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
#include <set>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// SpoolInfo Struct Tests
// ============================================================================

TEST_CASE("SpoolInfo - remaining_percent calculation", "[filament]") {
    SpoolInfo spool;

    SECTION("Full spool returns 100%") {
        spool.initial_weight_g = 1000.0;
        spool.remaining_weight_g = 1000.0;
        REQUIRE(spool.remaining_percent() == Catch::Approx(100.0));
    }

    SECTION("Half spool returns 50%") {
        spool.initial_weight_g = 1000.0;
        spool.remaining_weight_g = 500.0;
        REQUIRE(spool.remaining_percent() == Catch::Approx(50.0));
    }

    SECTION("Empty spool returns 0%") {
        spool.initial_weight_g = 1000.0;
        spool.remaining_weight_g = 0.0;
        REQUIRE(spool.remaining_percent() == Catch::Approx(0.0));
    }

    SECTION("Partial spool calculates correctly") {
        spool.initial_weight_g = 1000.0;
        spool.remaining_weight_g = 850.0;
        REQUIRE(spool.remaining_percent() == Catch::Approx(85.0));
    }

    SECTION("Non-standard spool weight works") {
        spool.initial_weight_g = 750.0; // 750g spool
        spool.remaining_weight_g = 500.0;
        REQUIRE(spool.remaining_percent() == Catch::Approx(66.666666).margin(0.001));
    }

    SECTION("Zero initial weight returns 0% (avoids division by zero)") {
        spool.initial_weight_g = 0.0;
        spool.remaining_weight_g = 100.0;
        REQUIRE(spool.remaining_percent() == Catch::Approx(0.0));
    }

    SECTION("Negative initial weight returns 0%") {
        spool.initial_weight_g = -100.0;
        spool.remaining_weight_g = 50.0;
        REQUIRE(spool.remaining_percent() == Catch::Approx(0.0));
    }
}

TEST_CASE("SpoolInfo - is_low threshold detection", "[filament]") {
    SpoolInfo spool;

    SECTION("Default threshold is 100g") {
        spool.remaining_weight_g = 99.0;
        REQUIRE(spool.is_low() == true);

        spool.remaining_weight_g = 100.0;
        REQUIRE(spool.is_low() == false);

        spool.remaining_weight_g = 101.0;
        REQUIRE(spool.is_low() == false);
    }

    SECTION("Custom threshold works") {
        spool.remaining_weight_g = 200.0;
        REQUIRE(spool.is_low(250.0) == true);
        REQUIRE(spool.is_low(200.0) == false);
        REQUIRE(spool.is_low(150.0) == false);
    }

    SECTION("Empty spool is always low") {
        spool.remaining_weight_g = 0.0;
        REQUIRE(spool.is_low() == true);
        REQUIRE(spool.is_low(0.0) == false); // Edge case: threshold 0
    }

    SECTION("Very low threshold") {
        spool.remaining_weight_g = 5.0;
        REQUIRE(spool.is_low(10.0) == true);
        REQUIRE(spool.is_low(5.0) == false);
        REQUIRE(spool.is_low(1.0) == false);
    }
}

TEST_CASE("SpoolInfo - display_name formatting", "[filament]") {
    SpoolInfo spool;

    SECTION("Full info formats correctly") {
        spool.vendor = "Polymaker";
        spool.material = "PLA";
        spool.filament_name = "Jet Black";
        REQUIRE(spool.display_name() == "Polymaker Jet Black PLA");
    }

    SECTION("Empty filament name is simply absent") {
        spool.vendor = "eSUN";
        spool.material = "PETG";
        spool.filament_name = "";
        REQUIRE(spool.display_name() == "eSUN PETG");
    }

    SECTION("No vendor omits vendor") {
        spool.vendor = "";
        spool.material = "ABS";
        spool.filament_name = "Red";
        REQUIRE(spool.display_name() == "Red ABS");
    }

    SECTION("Only material") {
        spool.vendor = "";
        spool.material = "TPU";
        spool.filament_name = "";
        REQUIRE(spool.display_name() == "TPU");
    }

    SECTION("Empty info returns 'Unknown Spool'") {
        spool.vendor = "";
        spool.material = "";
        spool.filament_name = "";
        REQUIRE(spool.display_name() == "Unknown Spool");
    }

    SECTION("Only a filament name") {
        spool.vendor = "";
        spool.material = "";
        spool.filament_name = "Blue";
        REQUIRE(spool.display_name() == "Blue");
    }

    SECTION("Complex color names preserved") {
        spool.vendor = "Eryone";
        spool.material = "Silk PLA";
        spool.filament_name = "Gold/Silver/Copper Tri-Color";
        REQUIRE(spool.display_name() == "Eryone Gold/Silver/Copper Tri-Color Silk PLA");
    }
}

TEST_CASE("SpoolInfo - default initialization", "[filament]") {
    SpoolInfo spool;

    SECTION("All numeric fields default to 0") {
        REQUIRE(spool.id == 0);
        REQUIRE(spool.remaining_weight_g == 0.0);
        REQUIRE(spool.remaining_length_m == 0.0);
        REQUIRE(spool.spool_weight_g == 0.0);
        REQUIRE(spool.initial_weight_g == 0.0);
        REQUIRE(spool.nozzle_temp_min == 0);
        REQUIRE(spool.nozzle_temp_max == 0);
        REQUIRE(spool.nozzle_temp_recommended == 0);
        REQUIRE(spool.bed_temp_min == 0);
        REQUIRE(spool.bed_temp_max == 0);
        REQUIRE(spool.bed_temp_recommended == 0);
    }

    SECTION("Strings default to empty") {
        REQUIRE(spool.vendor.empty());
        REQUIRE(spool.material.empty());
        REQUIRE(spool.filament_name.empty());
        REQUIRE(spool.color_hex.empty());
    }

    SECTION("is_active defaults to false") {
        REQUIRE(spool.is_active == false);
    }
}

// ============================================================================
// FilamentUsageRecord Tests
// ============================================================================

TEST_CASE("FilamentUsageRecord - default initialization", "[filament]") {
    FilamentUsageRecord record;

    SECTION("All fields default correctly") {
        REQUIRE(record.spool_id == 0);
        REQUIRE(record.used_weight_g == 0.0);
        REQUIRE(record.used_length_m == 0.0);
        REQUIRE(record.print_filename.empty());
        REQUIRE(record.timestamp == 0.0);
    }
}

// ============================================================================
// VendorInfo Tests
// ============================================================================

TEST_CASE("VendorInfo - default initialization", "[filament]") {
    VendorInfo vendor;

    SECTION("All fields default correctly") {
        REQUIRE(vendor.id == 0);
        REQUIRE(vendor.name.empty());
        REQUIRE(vendor.url.empty());
    }
}

TEST_CASE("VendorInfo - display_name formatting", "[filament]") {
    VendorInfo vendor;

    SECTION("Name returns name") {
        vendor.name = "Hatchbox";
        REQUIRE(vendor.display_name() == "Hatchbox");
    }

    SECTION("Empty name returns Unknown Vendor") {
        REQUIRE(vendor.display_name() == "Unknown Vendor");
    }
}

// ============================================================================
// FilamentInfo Tests
// ============================================================================

TEST_CASE("FilamentInfo - default initialization", "[filament]") {
    FilamentInfo filament;

    SECTION("All numeric fields default correctly") {
        REQUIRE(filament.id == 0);
        REQUIRE(filament.vendor_id == 0);
        REQUIRE(filament.density == 0.0f);
        REQUIRE(filament.diameter == Catch::Approx(1.75f));
        REQUIRE(filament.weight == 0.0f);
        REQUIRE(filament.spool_weight == 0.0f);
        REQUIRE(filament.nozzle_temp_min == 0);
        REQUIRE(filament.nozzle_temp_max == 0);
        REQUIRE(filament.bed_temp_min == 0);
        REQUIRE(filament.bed_temp_max == 0);
    }

    SECTION("Strings default to empty") {
        REQUIRE(filament.vendor_name.empty());
        REQUIRE(filament.material.empty());
        REQUIRE(filament.filament_name.empty());
        REQUIRE(filament.color_hex.empty());
    }

    SECTION("Diameter defaults to 1.75mm") {
        REQUIRE(filament.diameter == Catch::Approx(1.75f));
    }
}

TEST_CASE("FilamentInfo - display_name formatting", "[filament]") {
    FilamentInfo filament;

    SECTION("Full info formats correctly") {
        filament.vendor_name = "Polymaker";
        filament.material = "PLA";
        filament.filament_name = "Jet Black";
        REQUIRE(filament.display_name() == "Polymaker Jet Black PLA");
    }

    SECTION("Empty filament name is simply absent") {
        filament.vendor_name = "eSUN";
        filament.material = "PETG";
        REQUIRE(filament.display_name() == "eSUN PETG");
    }

    SECTION("No vendor omits vendor") {
        filament.material = "ABS";
        filament.filament_name = "Red";
        REQUIRE(filament.display_name() == "Red ABS");
    }

    SECTION("Only material") {
        filament.material = "TPU";
        REQUIRE(filament.display_name() == "TPU");
    }

    SECTION("Empty returns Unknown Filament") {
        REQUIRE(filament.display_name() == "Unknown Filament");
    }
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
        api.spoolman_mock().set_mock_spoolman_enabled(false);

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

    SECTION("Exactly one spool is active by default") {
        api.spoolman().get_spoolman_spools(
            [&](const std::vector<SpoolInfo>& spools) {
                REQUIRE(spools.size() > 0);
                const int active_count = std::count_if(
                    spools.begin(), spools.end(), [](const SpoolInfo& s) { return s.is_active; });
                REQUIRE(active_count == 1);
            },
            [](const MoonrakerError&) {});
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

    SECTION("Updates is_active flag on spools") {
        // Set spool 3 as active
        api.spoolman().set_active_spool(3, []() {}, [](const MoonrakerError&) {});

        // Verify spool 3 has is_active=true, others false
        api.spoolman().get_spoolman_spools(
            [](const std::vector<SpoolInfo>& spools) {
                for (const auto& spool : spools) {
                    if (spool.id == 3) {
                        REQUIRE(spool.is_active == true);
                    } else {
                        REQUIRE(spool.is_active == false);
                    }
                }
            },
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
        data["url"] = "https://example.com";

        bool callback_called = false;
        api.spoolman().create_spoolman_vendor(
            data,
            [&](const VendorInfo& vendor) {
                callback_called = true;
                REQUIRE(vendor.id > 0);
                REQUIRE(vendor.name == "Test Vendor");
                REQUIRE(vendor.url == "https://example.com");
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

    SECTION("Deleting non-existent spool still succeeds") {
        bool callback_called = false;
        api.spoolman().delete_spoolman_spool(
            9999, [&]() { callback_called = true; }, [](const MoonrakerError&) {});

        REQUIRE(callback_called);
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
    // Mirrors the real proxy's failure mode when Spoolman isn't connected:
    // these calls must error out, not silently succeed with mock data — the
    // slot editor's managed-state gating depends on this (HELIX_MOCK_SPOOLMAN=0
    // regression, see AmsEditOverlay::update_ui()/enter_spool_edit()).
    MoonrakerClientMock client;
    PrinterState state;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().set_mock_spoolman_enabled(false);

    SECTION("get_spoolman_spool errors, no spool delivered") {
        bool error_called = false;
        bool success_called = false;
        api.spoolman().get_spoolman_spool(
            1, [&](const std::optional<SpoolInfo>&) { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
    }

    SECTION("get_spoolman_spools errors, no list delivered") {
        bool error_called = false;
        bool success_called = false;
        api.spoolman().get_spoolman_spools(
            [&](const std::vector<SpoolInfo>&) { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
    }

    SECTION("update_spoolman_spool errors, no write applied") {
        bool error_called = false;
        bool success_called = false;
        nlohmann::json patch;
        patch["remaining_weight"] = 1.0;
        api.spoolman().update_spoolman_spool(
            1, patch, [&]() { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
    }

    SECTION("update_spoolman_filament errors, no write applied") {
        bool error_called = false;
        bool success_called = false;
        nlohmann::json patch;
        patch["color_hex"] = "00FF00";
        api.spoolman().update_spoolman_filament(
            300, patch, [&]() { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
    }

    SECTION("set_active_spool errors, no active change") {
        const int before = api.spoolman_mock().get_mock_active_spool_id();
        bool error_called = false;
        bool success_called = false;
        api.spoolman().set_active_spool(
            9, [&]() { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
        CHECK(api.spoolman_mock().get_mock_active_spool_id() == before);
    }

    SECTION("update_spoolman_spool_weight errors, no write applied") {
        bool error_called = false;
        bool success_called = false;
        api.spoolman().update_spoolman_spool_weight(
            1, 500.0, [&]() { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
    }

    SECTION("update_spoolman_filament_color errors, no write applied") {
        bool error_called = false;
        bool success_called = false;
        api.spoolman().update_spoolman_filament_color(
            300, "00FF00", [&]() { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
    }

    SECTION("get_spoolman_vendors errors, no list delivered") {
        bool error_called = false;
        bool success_called = false;
        api.spoolman().get_spoolman_vendors(
            [&](const std::vector<VendorInfo>&) { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
    }

    SECTION("get_spoolman_filaments errors, no list delivered") {
        bool error_called = false;
        bool success_called = false;
        api.spoolman().get_spoolman_filaments(
            [&](const std::vector<FilamentInfo>&) { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
    }

    SECTION("get_spoolman_filaments(vendor_id) errors, no list delivered") {
        bool error_called = false;
        bool success_called = false;
        api.spoolman().get_spoolman_filaments(
            2, [&](const std::vector<FilamentInfo>&) { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
    }

    SECTION("create_spoolman_vendor errors, no vendor created") {
        bool error_called = false;
        bool success_called = false;
        nlohmann::json vendor;
        vendor["name"] = "Acme";
        api.spoolman().create_spoolman_vendor(
            vendor, [&](const VendorInfo&) { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
        CHECK(api.spoolman_mock().created_vendors.empty());
    }

    SECTION("create_spoolman_filament errors, no filament created") {
        bool error_called = false;
        bool success_called = false;
        nlohmann::json filament;
        filament["material"] = "PLA";
        api.spoolman().create_spoolman_filament(
            filament, [&](const FilamentInfo&) { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
        CHECK(api.spoolman_mock().created_filaments.empty());
    }

    SECTION("create_spoolman_spool errors, no spool created") {
        bool error_called = false;
        bool success_called = false;
        nlohmann::json spool;
        spool["filament_id"] = 1;
        api.spoolman().create_spoolman_spool(
            spool, [&](const SpoolInfo&) { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
        CHECK(api.spoolman_mock().created_spools.empty());
    }

    SECTION("delete_spoolman_spool errors, no delete applied") {
        bool error_called = false;
        bool success_called = false;
        api.spoolman().delete_spoolman_spool(
            1, [&]() { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
    }

    SECTION("delete_spoolman_vendor errors, no delete applied") {
        bool error_called = false;
        bool success_called = false;
        api.spoolman().delete_spoolman_vendor(
            1, [&]() { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
    }

    SECTION("delete_spoolman_filament errors, no delete applied") {
        bool error_called = false;
        bool success_called = false;
        api.spoolman().delete_spoolman_filament(
            1, [&]() { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
    }

    SECTION("get_spoolman_external_vendors errors, no list delivered") {
        bool error_called = false;
        bool success_called = false;
        api.spoolman().get_spoolman_external_vendors(
            [&](const std::vector<VendorInfo>&) { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
    }

    SECTION("get_spoolman_external_filaments errors, no list delivered") {
        bool error_called = false;
        bool success_called = false;
        api.spoolman().get_spoolman_external_filaments(
            "Hatchbox", [&](const std::vector<FilamentInfo>&) { success_called = true; },
            [&](const MoonrakerError& err) {
                error_called = true;
                CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
            });
        CHECK(error_called);
        CHECK_FALSE(success_called);
    }
}

TEST_CASE("SpoolInfo - new fields have defaults", "[filament]") {
    SpoolInfo spool;

    REQUIRE(spool.price == 0.0);
    REQUIRE(spool.lot_nr.empty());
    REQUIRE(spool.comment.empty());
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
// parse_spool_info — nozzle/bed temperature RANGES
//
// apply_spool_to_slot() copies spool.nozzle_temp_min/max straight onto the
// slot, so a parser that never reads settings_extruder_temp_min/max hands every
// Spoolman-sourced slot a 0/0 nozzle range while the bed temperature is real.
// parse_filament_info() already reads all four keys; the spool path must agree.
// ============================================================================

TEST_CASE("parse_spool_info parses nozzle and bed temperature ranges",
          "[filament][parsing][spoolman]") {
    using helix::spoolman_detail::parse_spool_info;

    SECTION("min/max populate the range alongside the recommended value") {
        auto j = nlohmann::json::parse(R"({
            "id": 21,
            "filament": {
                "id": 4,
                "material": "PETG",
                "settings_extruder_temp": 240,
                "settings_extruder_temp_min": 230,
                "settings_extruder_temp_max": 250,
                "settings_bed_temp": 80,
                "settings_bed_temp_min": 70,
                "settings_bed_temp_max": 90
            }
        })");

        auto info = parse_spool_info(j);
        CHECK(info.nozzle_temp_recommended == 240);
        CHECK(info.nozzle_temp_min == 230);
        CHECK(info.nozzle_temp_max == 250);
        CHECK(info.bed_temp_recommended == 80);
        CHECK(info.bed_temp_min == 70);
        CHECK(info.bed_temp_max == 90);
    }

    SECTION("present-but-null min/max do not throw and read as 0") {
        // Spoolman serializes every optional numeric as null rather than
        // omitting it — a raw .value() on these throws type_error.302 and
        // aborts the whole spool-list parse (#1087).
        auto j = nlohmann::json::parse(R"({
            "id": 22,
            "filament": {
                "material": "PLA",
                "settings_extruder_temp_min": null,
                "settings_extruder_temp_max": null,
                "settings_bed_temp_min": null,
                "settings_bed_temp_max": null
            }
        })");

        SpoolInfo info;
        REQUIRE_NOTHROW(info = parse_spool_info(j));
        CHECK(info.nozzle_temp_min == 0);
        CHECK(info.nozzle_temp_max == 0);
        CHECK(info.bed_temp_min == 0);
        CHECK(info.bed_temp_max == 0);
    }

    SECTION("the range reaches the slot through apply_spool_to_slot") {
        // The consumer that made the omission user-visible: a slot linked to a
        // Spoolman spool showed a real bed temperature next to a 0/0 nozzle
        // range, because only the bed value was ever parsed.
        auto j = nlohmann::json::parse(R"({
            "id": 23,
            "filament": {
                "material": "PETG",
                "settings_extruder_temp": 240,
                "settings_extruder_temp_min": 230,
                "settings_extruder_temp_max": 250,
                "settings_bed_temp": 80,
                "settings_bed_temp_min": 70,
                "settings_bed_temp_max": 90
            }
        })");

        SlotInfo slot;
        apply_spool_to_slot(slot, parse_spool_info(j));
        CHECK(slot.nozzle_temp_min == 230);
        CHECK(slot.nozzle_temp_max == 250);
        CHECK(slot.bed_temp == 80);
    }
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

TEST_CASE("filter_spools - empty query returns all", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    auto result = filter_spools(spools, "");
    REQUIRE(result.size() == spools.size());
}

TEST_CASE("filter_spools - whitespace-only query returns all", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    auto result = filter_spools(spools, "   ");
    REQUIRE(result.size() == spools.size());
}

TEST_CASE("filter_spools - single term matches material", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    auto result = filter_spools(spools, "PLA");
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].id == 1);
    REQUIRE(result[1].id == 42);
}

TEST_CASE("filter_spools - single term matches vendor", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    auto result = filter_spools(spools, "polymaker");
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].id == 1);
    REQUIRE(result[1].id == 3);
}

TEST_CASE("filter_spools - multi-term AND matching", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    auto result = filter_spools(spools, "polymaker pla");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].id == 1);
}

TEST_CASE("filter_spools - case insensitive", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    auto result = filter_spools(spools, "ESUN petg");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].id == 2);
}

TEST_CASE("filter_spools - spool ID search with #", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    auto result = filter_spools(spools, "#42");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].id == 42);
}

TEST_CASE("filter_spools - spool ID search without #", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    // "42" matches spool #42's searchable text which contains "#42"
    auto result = filter_spools(spools, "42");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].id == 42);
}

TEST_CASE("filter_spools - color name search", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    auto result = filter_spools(spools, "blue");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].id == 2);
}

TEST_CASE("filter_spools - no matches returns empty", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    auto result = filter_spools(spools, "nonexistent");
    REQUIRE(result.empty());
}

TEST_CASE("filter_spools - empty spool list returns empty", "[filament][filter]") {
    std::vector<SpoolInfo> empty;
    auto result = filter_spools(empty, "PLA");
    REQUIRE(result.empty());
}

TEST_CASE("filter_spools - location search", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    auto result = filter_spools(spools, "shelf");
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].id == 1);
    REQUIRE(result[1].id == 3);
}

TEST_CASE("filter_spools - location + material AND search", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    auto result = filter_spools(spools, "shelf pla");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].id == 1);
}

TEST_CASE("filter_spools - empty location does not break search", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    // Spool s2 and s4 have empty location — they should still match on other fields
    auto result = filter_spools(spools, "hatchbox");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].id == 42);
}

// ============================================================================
// Fuzzy search tests
// ============================================================================

TEST_CASE("filter_spools - fuzzy matches vendor with typo", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    // "Polmaker" is 1 edit from "Polymaker" (missing 'y')
    auto result = filter_spools(spools, "polmaker");
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].id == 1);
    REQUIRE(result[1].id == 3);
}

TEST_CASE("filter_spools - fuzzy matches material with typo", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    // "PEYG" is 1 edit from "PETG" (Y instead of T)
    auto result = filter_spools(spools, "peyg");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].id == 2);
}

TEST_CASE("filter_spools - fuzzy matches vendor with swapped chars", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    // "htachbox" is 2 edits from "hatchbox" (transposition)
    auto result = filter_spools(spools, "htachbox");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].id == 42);
}

TEST_CASE("filter_spools - fuzzy does not match wildly different terms", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    // "xyz" is far from any word in the spool data
    auto result = filter_spools(spools, "xyz");
    REQUIRE(result.empty());
}

TEST_CASE("filter_spools - fuzzy AND still works with mixed exact and fuzzy",
          "[filament][filter]") {
    auto spools = make_filter_test_spools();
    // "polmaker" fuzzy-matches "polymaker", "pla" exact-matches
    auto result = filter_spools(spools, "polmaker pla");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].id == 1);
}

TEST_CASE("filter_spools - fuzzy short term has tighter threshold", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    // "PLS" is 1 edit from "PLA" — within threshold for 3-char terms
    auto result = filter_spools(spools, "pls");
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].id == 1);
    REQUIRE(result[1].id == 42);
}

TEST_CASE("filter_spools - fuzzy rejects 2-edit on short term", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    // "XYA" is 2 edits from "PLA" — exceeds threshold of 1 for 3-char terms
    auto result = filter_spools(spools, "xya");
    REQUIRE(result.empty());
}

TEST_CASE("filter_spools - exact substring still preferred over fuzzy", "[filament][filter]") {
    auto spools = make_filter_test_spools();
    // "poly" is an exact substring of "polymaker" — should match without needing fuzzy
    auto result = filter_spools(spools, "poly");
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].id == 1);
    REQUIRE(result[1].id == 3);
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

TEST_CASE("SpoolInfo - location defaults to empty string", "[filament]") {
    SpoolInfo spool;
    REQUIRE(spool.location.empty());
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

    auto& spools = api.spoolman_mock().get_mock_spools();
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

TEST_CASE("FilamentEntry defaults include diameter", "[spoolman]") {
    SpoolWizardOverlay::FilamentEntry entry;
    REQUIRE(entry.diameter == Catch::Approx(1.75));
    REQUIRE(entry.density == 0.0);
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

TEST_CASE("SpoolInfo - realistic spool scenarios", "[filament][integration]") {
    SECTION("Typical PLA spool usage") {
        SpoolInfo spool;
        spool.vendor = "Polymaker";
        spool.material = "PLA";
        spool.filament_name = "Jet Black";
        spool.color_hex = "1A1A2E";
        spool.initial_weight_g = 1000.0;
        spool.remaining_weight_g = 850.0;
        spool.nozzle_temp_recommended = 210;
        spool.bed_temp_recommended = 60;

        REQUIRE(spool.remaining_percent() == Catch::Approx(85.0));
        REQUIRE(spool.is_low() == false);
        REQUIRE(spool.is_low(900.0) == true); // Custom threshold
        REQUIRE(spool.display_name() == "Polymaker Jet Black PLA");
    }

    SECTION("Nearly empty ASA spool") {
        SpoolInfo spool;
        spool.vendor = "Flashforge";
        spool.material = "ASA";
        spool.filament_name = "Fire Engine Red";
        spool.initial_weight_g = 1000.0;
        spool.remaining_weight_g = 50.0;

        REQUIRE(spool.remaining_percent() == Catch::Approx(5.0));
        REQUIRE(spool.is_low() == true);
        REQUIRE(spool.is_low(50.0) == false);
    }

    SECTION("Engineering filament with 750g spool") {
        SpoolInfo spool;
        spool.vendor = "Polymaker";
        spool.material = "PC";
        spool.filament_name = "PolyMax PC Grey";
        spool.initial_weight_g = 750.0;
        spool.remaining_weight_g = 500.0;
        spool.nozzle_temp_recommended = 270;
        spool.bed_temp_recommended = 100;

        REQUIRE(spool.remaining_percent() == Catch::Approx(66.666666).margin(0.001));
        REQUIRE(spool.is_low() == false);
    }
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
    auto& spools = api.spoolman_mock().get_mock_spools();
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

TEST_CASE("SpoolInfo vendor_id defaults to 0 when not set", "[filament][spoolman]") {
    SpoolInfo spool;
    REQUIRE(spool.vendor_id == 0);
}

TEST_CASE("SlotInfo spoolman_vendor_id defaults to 0", "[ams][spoolman]") {
    SlotInfo slot;
    REQUIRE(slot.spoolman_vendor_id == 0);
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

TEST_CASE("sort_spools_by_recency puts a newly added never-used spool on top (#1071)",
          "[filament][spoolman][sort]") {
    // The user's exact scenario: a spool registered today but never used must
    // outrank a spool used yesterday. Under the old "used spools always first"
    // rule the brand-new spool sank to the bottom of the picker.
    std::vector<SpoolInfo> spools;
    spools.push_back(dated_spool(11, "2026-07-18T09:00:00", "2026-01-02T00:00:00"));
    spools.push_back(dated_spool(21, "", "2026-07-19T08:00:00")); // added today, never used

    sort_spools_by_recency(spools);

    CHECK(ids_of(spools) == std::vector<int>{21, 11});
}

TEST_CASE("sort_spools_by_recency puts a used spool above an older-registered unused spool",
          "[filament][spoolman][sort]") {
    std::vector<SpoolInfo> spools;
    spools.push_back(dated_spool(30, "", "2026-07-15T00:00:00")); // never used, older
    spools.push_back(dated_spool(31, "2026-07-19T00:00:00", "2026-03-01T00:00:00"));

    sort_spools_by_recency(spools);

    CHECK(ids_of(spools) == std::vector<int>{31, 30});
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

TEST_CASE("sort_spools_by_recency falls back to registered when last_used is missing",
          "[filament][spoolman][sort]") {
    std::vector<SpoolInfo> spools;
    spools.push_back(dated_spool(50, "", "2026-02-01T00:00:00"));
    spools.push_back(dated_spool(51, "", "2026-08-01T00:00:00"));
    spools.push_back(dated_spool(52, "", "2026-05-01T00:00:00"));

    sort_spools_by_recency(spools);

    // Never-used spools order among themselves by registration, newest first.
    CHECK(ids_of(spools) == std::vector<int>{51, 52, 50});
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
    CHECK(spool_recency_key(spools[1]) == SPOOL_RECENCY_NONE);
    CHECK(spool_recency_key(spools[2]) == SPOOL_RECENCY_NONE);
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

TEST_CASE("sort_spools_by_recency handles an empty list", "[filament][spoolman][sort]") {
    std::vector<SpoolInfo> spools;
    sort_spools_by_recency(spools);
    CHECK(spools.empty());
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
