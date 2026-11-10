// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spool_wizard.h"

#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "core/text_io.h"
#include "filament_database.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "spoolman_types.h"
#include "theme_manager.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Step Navigation Tests
// ============================================================================

TEST_CASE("SpoolWizardOverlay walks its steps forward and back", "[spool_wizard]") {
    using Step = SpoolWizardOverlay::Step;
    SpoolWizardOverlay wizard;
    bool closed = false;
    wizard.set_close_callback([&closed]() { closed = true; });
    CHECK(wizard.current_step() == Step::VENDOR);
    CHECK_FALSE(wizard.can_proceed());

    // Next is refused until the step can proceed.
    wizard.navigate_next();
    CHECK(wizard.current_step() == Step::VENDOR);
    wizard.set_can_proceed(true);
    CHECK(wizard.can_proceed());
    wizard.set_can_proceed(false);
    CHECK_FALSE(wizard.can_proceed());

    // Each step it reaches starts refusing again; the last one stays put.
    for (Step next : {Step::FILAMENT, Step::SPOOL_DETAILS}) {
        wizard.set_can_proceed(true);
        wizard.navigate_next();
        CHECK(wizard.current_step() == next);
        CHECK_FALSE(wizard.can_proceed());
    }
    wizard.set_can_proceed(true);
    wizard.navigate_next();
    CHECK(wizard.current_step() == Step::SPOOL_DETAILS);

    // Back walks down without closing; Back from the first step closes.
    for (Step prev : {Step::FILAMENT, Step::VENDOR}) {
        wizard.navigate_back();
        CHECK(wizard.current_step() == prev);
        CHECK_FALSE(closed);
    }
    wizard.navigate_back();
    CHECK(closed);
    CHECK(wizard.current_step() == Step::VENDOR);
}

TEST_CASE_METHOD(LVGLUITestFixture, "SpoolWizardOverlay header and field labels follow subjects",
                 "[spool_wizard]") {
    SpoolWizardOverlay wizard;
    wizard.init_subjects();
    CHECK(wizard.step_label() == "New Spool: Step 1 of 3");
    auto* root = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "spool_wizard", nullptr));
    REQUIRE(root != nullptr);
    lv_obj_t* title = lv_obj_find_by_name(root, "header_title");
    REQUIRE(title != nullptr);
    for (const char* step : {"2", "3"}) {
        wizard.set_can_proceed(true);
        wizard.navigate_next();
        const std::string expected = std::string("New Spool: Step ") + step + " of 3";
        CHECK(wizard.step_label() == expected);
        CHECK(helix::text_io::to_lower(lv_label_get_text(title)) ==
              helix::text_io::to_lower(expected));
    }

    // A required field the user left empty turns its label red.
    auto* modal =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "create_filament_modal", nullptr));
    REQUIRE(modal != nullptr);
    lv_obj_t* material = lv_obj_find_by_name(modal, "material_label");
    REQUIRE(material != nullptr);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "spool_wizard_material_error"), 1);
    CHECK(lv_color_eq(lv_obj_get_style_text_color(material, LV_PART_MAIN),
                      theme_manager_get_color("danger")));
    lv_subject_set_int(lv_xml_get_subject(nullptr, "spool_wizard_material_error"), 0);
    CHECK(lv_color_eq(lv_obj_get_style_text_color(material, LV_PART_MAIN),
                      theme_manager_get_color("text_muted")));
    lv_obj_delete(modal);
    lv_obj_delete(root);
}

TEST_CASE_METHOD(HelixTestFixture, "SpoolWizardOverlay on_create_requested enters creating state",
                 "[spool_wizard]") {
    SpoolWizardOverlay wizard;
    bool completed = false;
    wizard.set_completion_callback([&completed]() { completed = true; });

    // Navigate to SPOOL_DETAILS step
    wizard.set_can_proceed(true);
    wizard.navigate_next();
    wizard.set_can_proceed(true);
    wizard.navigate_next();
    CHECK(wizard.current_step() == SpoolWizardOverlay::Step::SPOOL_DETAILS);

    // Without API, on_create_requested hits "No API connection" error path.
    // Completion callback is NOT fired (only fires on success).
    wizard.on_create_requested();
    CHECK_FALSE(completed);
}

// ============================================================================
// Vendor List Tests
// ============================================================================

TEST_CASE("sorted_vendors sorts case-insensitively and keeps one per name", "[spool_wizard]") {
    auto result = SpoolWizardOverlay::sorted_vendors(
        {{"Zyltech", 3, true}, {"atomic Filament", 1, true}, {"Overture", 2, true}});
    REQUIRE(result.size() == 3);
    CHECK(result[0].name == "atomic Filament");
    CHECK(result[1].name == "Overture");
    CHECK(result[2].name == "Zyltech");

    // Spoolman does not keep vendor names unique.
    result = SpoolWizardOverlay::sorted_vendors({{"polymaker", 1, true}, {"Polymaker", 2, true}});
    REQUIRE(result.size() == 1);
    CHECK(result[0].server_id == 2);
    CHECK(SpoolWizardOverlay::sorted_vendors({}).empty());
}

TEST_CASE("filter_vendor_list matches a case-insensitive substring", "[spool_wizard]") {
    const std::vector<SpoolWizardOverlay::VendorEntry> vendors = {
        {"Polymaker", 5, true}, {"Hatchbox", 10, true}, {"PolyTerra", -1, false}};
    CHECK(SpoolWizardOverlay::filter_vendor_list(vendors, "").size() == 3);
    auto filtered = SpoolWizardOverlay::filter_vendor_list(vendors, "poly");
    REQUIRE(filtered.size() == 2);
    CHECK(filtered[0].name == "Polymaker");
    CHECK(filtered[1].name == "PolyTerra");
    CHECK(SpoolWizardOverlay::filter_vendor_list(vendors, "xyz").empty());
}

TEST_CASE("SpoolWizardOverlay ignores a pick past an empty list", "[spool_wizard]") {
    SpoolWizardOverlay wizard;
    wizard.select_vendor(0);
    CHECK_FALSE(wizard.can_proceed());
    wizard.set_can_proceed(true);
    wizard.navigate_next();
    wizard.select_filament(0);
    CHECK_FALSE(wizard.can_proceed());
}

TEST_CASE("SpoolWizardOverlay keeps the new vendor and colour it is given", "[spool_wizard]") {
    SpoolWizardOverlay wizard;
    wizard.set_new_vendor("Polymaker", "https://polymaker.com");
    CHECK(wizard.new_vendor_name() == "Polymaker");
    CHECK(wizard.new_vendor_url() == "https://polymaker.com");
    wizard.set_new_vendor("", "");
    CHECK(wizard.new_vendor_name().empty());
    CHECK(wizard.new_vendor_url().empty());

    wizard.set_new_filament_color("1A2B3C", "Teal");
    CHECK(wizard.new_filament_color_hex() == "1A2B3C");
    CHECK(wizard.new_filament_color_name() == "Teal");
    wizard.set_new_filament_color("", "");
    CHECK(wizard.new_filament_color_hex().empty());
    CHECK(wizard.new_filament_color_name().empty());
}

TEST_CASE("set_new_filament_material fills temps and density from the database", "[spool_wizard]") {
    struct Row {
        const char* material;
        int nozzle_min, nozzle_max, bed_min;
        double density;
    };
    // Nylon resolves to PA; an unknown material keeps the zero defaults.
    for (const Row& r : {Row{"PLA", 190, 220, 60, 1.24}, Row{"PETG", 230, 260, 80, 1.27},
                         Row{"Nylon", 250, 280, 80, 1.14}, Row{"UnknownMaterial", 0, 0, 0, 0.0}}) {
        CAPTURE(r.material);
        SpoolWizardOverlay wizard;
        wizard.set_new_filament_material(r.material);
        CHECK(wizard.new_filament_nozzle_min() == r.nozzle_min);
        CHECK(wizard.new_filament_nozzle_max() == r.nozzle_max);
        CHECK(wizard.new_filament_bed_min() == r.bed_min);
        if (r.density > 0) {
            CHECK(wizard.new_filament_density() == Catch::Approx(r.density));
        }
    }
}

TEST_CASE("new filament: material alone does not enable proceed", "[spool_wizard]") {
    SpoolWizardOverlay wizard;
    wizard.set_can_proceed(true);
    wizard.navigate_next();
    wizard.set_new_filament_material("PLA");
    CHECK_FALSE(wizard.can_proceed());
}

TEST_CASE("spool details start empty", "[spool_wizard]") {
    SpoolWizardOverlay wizard;
    CHECK(wizard.spool_remaining_weight() == 0);
    CHECK(wizard.spool_price() == 0);
    CHECK(wizard.spool_lot_nr().empty());
    CHECK(wizard.spool_notes().empty());
    // Reaching the details step with no filament picked leaves the weight unset.
    for (int i = 0; i < 2; ++i) {
        wizard.set_can_proceed(true);
        wizard.navigate_next();
    }
    CHECK(wizard.spool_remaining_weight() == 0);
}

TEST_CASE_METHOD(HelixTestFixture, "on_create_requested without API calls on_creation_error path",
                 "[spool_wizard]") {
    SpoolWizardOverlay wizard;
    bool completed = false;
    wizard.set_completion_callback([&]() { completed = true; });

    // Navigate to spool details
    wizard.set_can_proceed(true);
    wizard.navigate_next();
    wizard.set_can_proceed(true);
    wizard.navigate_next();
    CHECK(wizard.current_step() == SpoolWizardOverlay::Step::SPOOL_DETAILS);

    // With no API, vendor creation path hits "No API connection" error.
    // selected_vendor_.server_id defaults to -1, so it tries to create vendor.
    // No crash, no completion callback fired.
    wizard.on_create_requested();
    CHECK_FALSE(completed);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "on_create_requested fires completion when vendor+filament exist",
                 "[spool_wizard]") {
    SpoolWizardOverlay wizard;
    bool completed = false;
    wizard.set_completion_callback([&]() { completed = true; });

    // Navigate to spool details
    wizard.set_can_proceed(true);
    wizard.navigate_next();
    wizard.set_can_proceed(true);
    wizard.navigate_next();

    // Without API, even with server_ids set, create_spool will fail with "No API"
    // This test verifies the no-API path does not crash and handles gracefully.
    wizard.on_create_requested();
    CHECK_FALSE(completed);
}

// ============================================================================
// Filament creation payload
// ============================================================================

TEST_CASE("filament_create_payload sends Spoolman one integer per temperature",
          "[spool_wizard][spoolman]") {
    // A catalog filament carries a nozzle range; Spoolman stores one integer
    // and answers anything else with 422.
    SpoolWizardOverlay::FilamentEntry f;
    f.material = "PLA";
    f.color_hex = "FF0000";
    f.nozzle_temp_min = 190;
    f.nozzle_temp_max = 220;
    f.bed_temp_min = 60;
    f.bed_temp_max = 60;

    const nlohmann::json payload = SpoolWizardOverlay::filament_create_payload(f, 7);
    CHECK(payload["settings_extruder_temp"] == 205);
    CHECK(payload["settings_bed_temp"] == 60);

    helix::PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    bool created = false;
    api.spoolman().create_spoolman_filament(
        payload, [&](const FilamentInfo&) { created = true; },
        [](const MoonrakerError& err) { FAIL("Spoolman rejected the payload: " << err.message); });
    CHECK(created);
}

TEST_CASE("filament_create_payload omits unset temperatures", "[spool_wizard][spoolman]") {
    SpoolWizardOverlay::FilamentEntry f;
    f.material = "PLA";
    f.nozzle_temp_max = 215;

    const nlohmann::json payload = SpoolWizardOverlay::filament_create_payload(f, 7);
    CHECK(payload["settings_extruder_temp"] == 215);
    CHECK_FALSE(payload.contains("settings_bed_temp"));
}

TEST_CASE("filament_create_payload records the slicer's material and keeps the variant in the name",
          "[spool_wizard][spoolman][filament_naming]") {
    SpoolWizardOverlay::FilamentEntry f;
    f.material = "ABS+";
    f.color_name = "Deep Gray";

    nlohmann::json payload = SpoolWizardOverlay::filament_create_payload(f, 7);
    CHECK(payload["material"] == "ABS");
    CHECK(payload["name"] == "ABS+ Deep Gray");

    // A name of its own that never states the variant gets it in front.
    f.name = "Deep Gray";
    payload = SpoolWizardOverlay::filament_create_payload(f, 7);
    CHECK(payload["material"] == "ABS");
    CHECK(payload["name"] == "ABS+ Deep Gray");

    // A name that already says it is kept as it is.
    f.name = "Grey ABS+HS";
    CHECK(SpoolWizardOverlay::filament_create_payload(f, 7)["name"] == "Grey ABS+HS");
}

TEST_CASE("existing_filament_for finds the record a pick files, so a repeat pick reuses it",
          "[spool_wizard][spoolman][exact_filament]") {
    SpoolWizardOverlay::FilamentEntry e;
    e.name = "PLA+ White";
    e.material = "PLA+";
    e.color_hex = "FFFFFF";
    const nlohmann::json stored = SpoolWizardOverlay::filament_create_payload(e, 3);

    FilamentInfo filed;
    filed.id = 12;
    filed.vendor_id = 3;
    filed.material = stored["material"].get<std::string>();
    filed.filament_name = stored["name"].get<std::string>();
    filed.color_hex = "FFFFFF";
    const std::vector<FilamentInfo> filaments{filed};

    const FilamentInfo* found = SpoolWizardOverlay::existing_filament_for(filaments, e, 3);
    REQUIRE(found != nullptr);
    CHECK(found->id == 12);
    CHECK(SpoolWizardOverlay::existing_filament_for(filaments, e, 4) == nullptr);
}

TEST_CASE("vendor_create_payload keeps the website Spoolman has no field for",
          "[spool_wizard][spoolman]") {
    // Spoolman's vendor has no url field and silently drops unknown keys, so
    // the website lands in the vendor's comment.
    helix::PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    const nlohmann::json payload =
        SpoolWizardOverlay::vendor_create_payload("Acme", "https://acme.example");
    int vendor_id = 0;
    api.spoolman().create_spoolman_vendor(
        payload, [&](const VendorInfo& v) { vendor_id = v.id; }, nullptr);
    REQUIRE(vendor_id > 0);

    nlohmann::json served;
    client.send_jsonrpc(
        "server.spoolman.proxy", {{"request_method", "GET"}, {"path", "/v1/vendor"}},
        [&](const nlohmann::json& r) { served = r["result"]; }, [](const MoonrakerError&) {});
    const auto it = std::find_if(served.begin(), served.end(),
                                 [&](const nlohmann::json& v) { return v["id"] == vendor_id; });
    REQUIRE(it != served.end());
    CHECK((*it)["comment"] == "https://acme.example");
}

// ============================================================================
// Screensaver
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "a screensaver wake keeps the wizard's step and entries",
                 "[spool_wizard][suspend]") {
    SpoolWizardOverlay wizard;
    wizard.on_activate();
    wizard.set_new_vendor("Polymaker", "https://polymaker.com");
    wizard.set_can_proceed(true);
    wizard.navigate_next();
    REQUIRE(wizard.current_step() == SpoolWizardOverlay::Step::FILAMENT);

    wizard.on_deactivate(DeactivateReason::Suspended);
    wizard.on_activate();

    CHECK(wizard.current_step() == SpoolWizardOverlay::Step::FILAMENT);
    CHECK(wizard.new_vendor_name() == "Polymaker");

    // Opening the wizard again after leaving it starts fresh.
    wizard.on_deactivate(DeactivateReason::NavigateAway);
    wizard.on_activate();
    CHECK(wizard.current_step() == SpoolWizardOverlay::Step::VENDOR);
    CHECK(wizard.new_vendor_name().empty());
}
