// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The spool wizard's SpoolmanDB search: the catalog-to-filament mapping, vendor
// and filament reuse, and the wizard's handling of late or missing answers.

#include "ui_search_debounce.h"
#include "ui_spool_wizard.h"

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "spoolman_catalog_search.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using helix::ExternalFilament;
using helix::SpoolmanCatalogSearch;

namespace {

ExternalFilament catalog_filament() {
    ExternalFilament f;
    f.id = "polymaker_petg_polylitepetgblue_1000_175_n";
    f.manufacturer = "Polymaker";
    f.name = "PolyLite PETG Blue";
    f.material = "PETG";
    f.color_hex = "1E5AA8";
    f.density = 1.25f;
    f.diameter = 1.75f;
    f.weight = 1000.0f;
    f.spool_weight = 140.0f;
    f.extruder_temp = 240;
    f.bed_temp = 75;
    return f;
}

/// A wizard wired to the mock API the way the app wires it.
struct WizardCatalogFixture : LVGLTestFixture {
    helix::PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api{client, state};
    SpoolWizardOverlay wizard;

    WizardCatalogFixture() {
        SpoolmanCatalogSearch::reset_cache();
        set_moonraker_api(&api);
    }
    ~WizardCatalogFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        set_moonraker_api(nullptr);
        SpoolmanCatalogSearch::reset_cache();
    }

    void drain() {
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE("entry_from_catalog pre-fills everything Spoolman's create path takes",
          "[spool_wizard][spoolman_db]") {
    SECTION("a single-colour filament") {
        const auto e = SpoolWizardOverlay::entry_from_catalog(catalog_filament());
        CHECK(e.name == "PolyLite PETG Blue");
        CHECK(e.material == "PETG");
        CHECK(e.color_hex == "1E5AA8");
        CHECK(e.multi_color_hexes.empty());
        CHECK(e.density == Catch::Approx(1.25));
        CHECK(e.diameter == Catch::Approx(1.75));
        CHECK(e.weight == Catch::Approx(1000.0));
        CHECK(e.spool_weight == Catch::Approx(140.0));
        CHECK(e.nozzle_temp_min == 240);
        CHECK(e.nozzle_temp_max == 240);
        CHECK(e.bed_temp_min == 75);
        CHECK(e.bed_temp_max == 75);
        CHECK(e.server_id == -1);

        const auto payload = SpoolWizardOverlay::filament_create_payload(e, 3);
        CHECK(payload["color_hex"] == "1E5AA8");
        CHECK(payload["settings_extruder_temp"] == 240);
        CHECK(payload["settings_bed_temp"] == 75);
        CHECK(payload["spool_weight"] == Catch::Approx(140.0));
    }

    SECTION("a multi-colour filament goes in Spoolman's multi_color_hexes") {
        ExternalFilament f = catalog_filament();
        f.color_hex.clear();
        f.color_hexes = {"E53935", "FFEB3B"};
        f.multi_color_direction = "coaxial";
        const auto e = SpoolWizardOverlay::entry_from_catalog(f);
        CHECK(e.color_hex == "E53935");
        CHECK(e.multi_color_hexes == "E53935,FFEB3B");

        const auto payload = SpoolWizardOverlay::filament_create_payload(e, 3);
        CHECK(payload["multi_color_hexes"] == "E53935,FFEB3B");
        CHECK(payload["multi_color_direction"] == "coaxial");
        CHECK_FALSE(payload.contains("color_hex"));
    }

    SECTION("an unknown diameter is the common 1.75 mm") {
        ExternalFilament f = catalog_filament();
        f.diameter = 0;
        CHECK(SpoolWizardOverlay::entry_from_catalog(f).diameter == Catch::Approx(1.75));
    }
}

TEST_CASE("find_server_vendor matches a server vendor by name, ignoring case",
          "[spool_wizard][spoolman_db]") {
    const std::vector<SpoolWizardOverlay::VendorEntry> vendors{
        {"Acme", -1, false}, {"POLYMAKER ", 7, true}, {"eSUN", 2, true}};
    CHECK(SpoolWizardOverlay::find_server_vendor(vendors, " Polymaker") == 1);
    CHECK(SpoolWizardOverlay::find_server_vendor(vendors, "esun") == 2);
    // A vendor only typed into the wizard is not on the server yet.
    CHECK(SpoolWizardOverlay::find_server_vendor(vendors, "acme") == -1);
    CHECK(SpoolWizardOverlay::find_server_vendor(vendors, "Prusament") == -1);
}

TEST_CASE_METHOD(WizardCatalogFixture,
                 "searches never stack: one in flight, the latest waiting, the superseded answer "
                 "dropped",
                 "[spool_wizard][spoolman_db]") {
    // The first query's answer is held back while three more are typed.
    client.defer_next("server.spoolman.proxy");
    const int before = client.call_count("server.spoolman.proxy");
    wizard.run_catalog_search("poly");
    wizard.run_catalog_search("esun");
    wizard.run_catalog_search("sunlu");
    wizard.run_catalog_search("prusament");
    drain();
    CHECK(client.call_count("server.spoolman.proxy") == before + 1);
    CHECK(wizard.catalog_results().empty());
    CHECK(wizard.catalog_state() == SpoolWizardOverlay::CatalogState::Loading);

    // The held answer lands: it is dropped, and only the latest query goes.
    client.fire_deferred("server.spoolman.proxy");
    drain();
    drain();
    CHECK(client.call_count("server.spoolman.proxy") == before + 2);
    REQUIRE_FALSE(wizard.catalog_results().empty());
    for (const auto& f : wizard.catalog_results()) {
        CHECK(f.manufacturer == "Prusament");
    }
    CHECK(wizard.catalog_state() == SpoolWizardOverlay::CatalogState::Results);
}

TEST_CASE_METHOD(WizardCatalogFixture, "a search shows at most a page of 25 results",
                 "[spool_wizard][spoolman_db]") {
    wizard.run_catalog_search("pla");
    drain();
    CHECK(wizard.catalog_results().size() ==
          static_cast<size_t>(SpoolmanCatalogSearch::kResultLimit));
}

TEST_CASE_METHOD(WizardCatalogFixture, "a short query sends nothing and clears the section",
                 "[spool_wizard][spoolman_db]") {
    const int before = client.spoolman_mock().external_search_count();
    wizard.run_catalog_search("p");
    drain();
    CHECK(client.spoolman_mock().external_search_count() == before);
    CHECK(wizard.catalog_state() == SpoolWizardOverlay::CatalogState::Idle);
}

TEST_CASE_METHOD(WizardCatalogFixture,
                 "an older Spoolman hides the search after one request per connection",
                 "[spool_wizard][spoolman_db]") {
    client.spoolman_mock().set_external_search_supported(false);

    wizard.run_catalog_search("poly");
    drain();
    CHECK(SpoolmanCatalogSearch::availability(client.connection_generation()) ==
          SpoolmanCatalogSearch::Availability::Unavailable);
    // Hidden, not an error: the server simply has no catalog search.
    CHECK(wizard.catalog_state() == SpoolWizardOverlay::CatalogState::Idle);

    const int asked = client.call_count("server.spoolman.proxy");
    wizard.run_catalog_search("prusa");
    drain();
    CHECK(client.call_count("server.spoolman.proxy") == asked);
}

TEST_CASE_METHOD(WizardCatalogFixture, "a failed search shows the error state",
                 "[spool_wizard][spoolman_db]") {
    client.fail_next("server.spoolman.proxy", MoonrakerError::unknown("timeout"));
    wizard.run_catalog_search("poly");
    drain();
    CHECK(wizard.catalog_state() == SpoolWizardOverlay::CatalogState::Error);
}

TEST_CASE_METHOD(WizardCatalogFixture,
                 "saving a SpoolmanDB pick reuses the server's vendor and filament",
                 "[spool_wizard][spoolman_db]") {
    // The server already has Polymaker, and the PETG Blue the catalog names.
    client.spoolman_mock().add_vendor(7, "Polymaker");
    client.spoolman_mock().add_filament(70, 7, "PETG", "1e5aa8", " polylite petg blue");
    wizard.load_vendors();
    drain();

    wizard.apply_catalog_results({catalog_filament()});
    wizard.select_catalog_result(0);
    drain();

    CHECK(wizard.selected_vendor().server_id == 7);
    CHECK(wizard.selected_filament().server_id == 70);
    CHECK(wizard.current_step() == SpoolWizardOverlay::Step::SPOOL_DETAILS);
    CHECK(wizard.spool_remaining_weight() == Catch::Approx(1000.0));

    wizard.on_create_requested();
    drain();
    CHECK(client.spoolman_mock().created_vendors.empty());
    CHECK(client.spoolman_mock().created_filaments.empty());
    REQUIRE(client.spoolman_mock().created_spools.size() == 1);
    CHECK(client.spoolman_mock().created_spools[0]["filament_id"] == 70);
}

TEST_CASE_METHOD(WizardCatalogFixture,
                 "saving a SpoolmanDB pick creates only what the server lacks",
                 "[spool_wizard][spoolman_db]") {
    client.spoolman_mock().add_vendor(7, "Polymaker");
    wizard.load_vendors();
    drain();

    SECTION("an existing vendor gets the new filament") {
        wizard.apply_catalog_results({catalog_filament()});
        wizard.select_catalog_result(0);
        drain();
        CHECK(wizard.selected_vendor().server_id == 7);
        CHECK(wizard.selected_filament().server_id == -1);

        wizard.on_create_requested();
        drain();
        CHECK(client.spoolman_mock().created_vendors.empty());
        REQUIRE(client.spoolman_mock().created_filaments.size() == 1);
        CHECK(client.spoolman_mock().created_filaments[0]["vendor_id"] == 7);
        CHECK(client.spoolman_mock().created_filaments[0]["settings_extruder_temp"] == 240);
    }

    SECTION("a vendor the server lacks is created with it") {
        ExternalFilament f = catalog_filament();
        f.manufacturer = "Prusament";
        wizard.apply_catalog_results({f});
        wizard.select_catalog_result(0);
        drain();
        CHECK(wizard.selected_vendor().server_id == -1);

        wizard.on_create_requested();
        drain();
        // The filament and spool follow the vendor only while the wizard is
        // on screen, which a unit test does not put it.
        REQUIRE(client.spoolman_mock().created_vendors.size() == 1);
        CHECK(client.spoolman_mock().created_vendors[0]["name"] == "Prusament");
    }
}

TEST_CASE_METHOD(WizardCatalogFixture, "an answer after the wizard closes is ignored",
                 "[spool_wizard][spoolman_db]") {
    client.defer_next("server.spoolman.proxy");
    wizard.run_catalog_search("poly");
    wizard.on_deactivating(DeactivateReason::NavigateAway);
    client.fire_deferred("server.spoolman.proxy");
    drain();
    CHECK(wizard.catalog_results().empty());
}

TEST_CASE_METHOD(WizardCatalogFixture, "a SpoolmanDB pick never lands on a different filament",
                 "[spool_wizard][spoolman_db]") {
    // Same vendor, material and colour, but another product line.
    client.spoolman_mock().add_vendor(7, "Polymaker");
    client.spoolman_mock().add_filament(70, 7, "PETG", "1E5AA8", "PolyMax PETG Blue");
    wizard.load_vendors();
    drain();

    wizard.apply_catalog_results({catalog_filament()});
    wizard.select_catalog_result(0);
    drain();
    CHECK(wizard.selected_vendor().server_id == 7);
    CHECK(wizard.selected_filament().server_id == -1);
}

TEST_CASE_METHOD(WizardCatalogFixture, "picking a result stops a search still settling",
                 "[spool_wizard][spoolman_db]") {
    client.spoolman_mock().add_vendor(7, "Polymaker");
    wizard.load_vendors();
    drain();
    wizard.apply_catalog_results({catalog_filament()});
    REQUIRE(wizard.catalog_state() == SpoolWizardOverlay::CatalogState::Results);

    // A keystroke is still debouncing when the row is tapped.
    wizard.on_search_key();
    wizard.select_catalog_result(0);
    drain();
    process_lvgl(static_cast<int>(2 * helix::ui::kDefaultSearchDebounceMs));
    drain();

    // The settled search never ran over the pick.
    CHECK(wizard.current_step() == SpoolWizardOverlay::Step::SPOOL_DETAILS);
    CHECK(wizard.catalog_state() == SpoolWizardOverlay::CatalogState::Results);
}

TEST_CASE_METHOD(WizardCatalogFixture,
                 "Back from a SpoolmanDB pick lands on the vendor's filaments",
                 "[spool_wizard][spoolman_db]") {
    client.spoolman_mock().add_vendor(7, "Polymaker");
    client.spoolman_mock().add_filament(70, 7, "PLA", "1A1A1A", "PolyLite PLA Black");
    wizard.load_vendors();
    drain();
    wizard.apply_catalog_results({catalog_filament()});
    wizard.select_catalog_result(0);
    drain();
    REQUIRE(wizard.current_step() == SpoolWizardOverlay::Step::SPOOL_DETAILS);

    wizard.navigate_back();
    CHECK(wizard.current_step() == SpoolWizardOverlay::Step::FILAMENT);
    CHECK(
        std::any_of(wizard.all_filaments().begin(), wizard.all_filaments().end(),
                    [](const SpoolWizardOverlay::FilamentEntry& e) { return e.server_id == 70; }));
}

TEST_CASE_METHOD(WizardCatalogFixture, "closing the wizard frees its search timer",
                 "[spool_wizard][spoolman_db]") {
    auto timer_count = []() {
        int n = 0;
        for (lv_timer_t* t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) {
            ++n;
        }
        return n;
    };
    process_lvgl(20);
    const int before = timer_count();
    for (int i = 0; i < 5; ++i) {
        wizard.on_search_key();
        wizard.on_deactivating(DeactivateReason::NavigateAway);
        process_lvgl(20);
    }
    CHECK(timer_count() <= before);
}
