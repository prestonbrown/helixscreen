// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Wiring tests for the printer-identify step's selector: search filtering,
// vendor drill-in and the auto-drill that lands a prior selection inside its
// vendor bucket. The filter/group rules themselves are pinned headless in
// test_ui_selector_model.cpp; these pin that the step's widgets follow them.

#include "ui_selector_model.h"
#include "ui_wizard.h"
#include "ui_wizard_printer_identify.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/one_line_label.h"
#include "../test_helpers/wizard_printer_identify_test_access.h"
#include "app_globals.h"
#include "config.h"
#include "lvgl/lvgl.h"
#include "moonraker_api.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::wizard::StepId;

namespace {

constexpr int kViewTiles = 0;
constexpr int kViewVendor = 1;
constexpr int kViewSearch = 2;
constexpr int kViewCandidates = 3;

std::vector<helix::ui::SelectorEntry> detector_entries() {
    std::vector<helix::ui::SelectorEntry> entries;
    const auto& list = PrinterDetector::get_list_entries("");
    entries.reserve(list.size());
    for (size_t i = 0; i < list.size(); ++i) {
        entries.push_back({list[i].name, list[i].manufacturer, static_cast<int>(i)});
    }
    return entries;
}

class WizardPrinterIdentifyUIFixture : public LVGLUITestFixture {
  public:
    WizardPrinterIdentifyUIFixture() {
        open();
    }

  protected:
    // For fixtures that prepare what the step detects before it opens.
    struct DeferOpen {};
    explicit WizardPrinterIdentifyUIFixture(DeferOpen) {}

    void open() {
        wizard = ui_wizard_create(test_screen());
        if (!wizard) {
            spdlog::error("[WizardPrinterIdentifyUIFixture] Failed to create wizard!");
            return;
        }
        lv_obj_t* content = lv_obj_find_by_name(wizard, "wizard_content");
        if (!content) {
            spdlog::warn(
                "[WizardPrinterIdentifyUIFixture] XML components not loaded, skipping navigation");
            return;
        }

        ui_wizard_navigate_to_step(StepId::PrinterIdentify);
        ready_ = (lv_obj_find_by_name(wizard, "printer_type_list") != nullptr) &&
                 (lv_obj_find_by_name(wizard, "vendor_tiles") != nullptr) &&
                 (lv_obj_find_by_name(wizard, "printer_search_input") != nullptr);
        step_root = wizard;
    }

  public:
    ~WizardPrinterIdentifyUIFixture() {
        if (ready_) {
            get_wizard_printer_identify_step()->cleanup();
        }
        // Do NOT call lv_obj_delete(wizard) - lv_deinit() in LVGLTestFixture
        // handles widget tree cleanup.
        wizard = nullptr;
    }

    void require_ready() {
        if (!ready_) {
            SKIP("XML infrastructure not available (ui_integration test)");
        }
    }

    // The step root the helpers search below. Tests that re-create the step
    // onto their own parent repoint this first.
    lv_obj_t* step_root = nullptr;

    lv_obj_t* rows_container() {
        return lv_obj_find_by_name(step_root, "printer_type_list");
    }

    lv_obj_t* tiles_container() {
        return lv_obj_find_by_name(step_root, "vendor_tiles");
    }

    lv_obj_t* search_input() {
        return lv_obj_find_by_name(step_root, "printer_search_input");
    }

    int view() {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, "wizard_printer_view");
        REQUIRE(subject != nullptr);
        return lv_subject_get_int(subject);
    }

    // Rows of printer_type_list in list order.
    std::vector<lv_obj_t*> rows() {
        std::vector<lv_obj_t*> result;
        lv_obj_t* container = rows_container();
        REQUIRE(container != nullptr);
        const uint32_t count = lv_obj_get_child_count(container);
        for (uint32_t i = 0; i < count; ++i) {
            result.push_back(lv_obj_get_child(container, static_cast<int32_t>(i)));
        }
        return result;
    }

    std::string row_name(lv_obj_t* row) {
        lv_obj_t* label = lv_obj_get_child(row, 0);
        REQUIRE(label != nullptr);
        return lv_label_get_text(label);
    }

    // Rows not hidden by the active view filter.
    std::vector<lv_obj_t*> visible_rows() {
        auto all = rows();
        all.erase(
            std::remove_if(all.begin(), all.end(),
                           [](lv_obj_t* row) { return lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN); }),
            all.end());
        return all;
    }

    lv_obj_t* tile_for(const std::string& vendor) {
        lv_obj_t* tiles = tiles_container();
        REQUIRE(tiles != nullptr);
        const uint32_t count = lv_obj_get_child_count(tiles);
        for (uint32_t i = 0; i < count; ++i) {
            lv_obj_t* tile = lv_obj_get_child(tiles, static_cast<int32_t>(i));
            lv_obj_t* label = lv_obj_get_child(tile, 0);
            if (label && lv_streq(lv_label_get_text(label), vendor.c_str())) {
                return tile;
            }
        }
        return nullptr;
    }

    lv_obj_t* wizard = nullptr;
    bool ready_ = false;
};

} // namespace

// ============================================================================
// Search
// ============================================================================

TEST_CASE_METHOD(WizardPrinterIdentifyUIFixture, "Wizard printer search filters visible rows",
                 "[wizard][selector]") {
    require_ready();

    lv_obj_t* input = search_input();
    REQUIRE(input != nullptr);

    const auto entries = detector_entries();
    REQUIRE(entries.size() > 10);

    SECTION("query matching a model keeps exactly the matching rows") {
        lv_textarea_set_text(input, "kobra 2");

        std::vector<std::string> expected;
        for (const auto& e : entries) {
            if (helix::ui::selector_entry_matches(e, "kobra 2")) {
                expected.push_back(e.label);
            }
        }
        REQUIRE_FALSE(expected.empty());

        REQUIRE(view() == kViewSearch);
        const auto shown = visible_rows();
        REQUIRE(shown.size() == expected.size());
        for (size_t i = 0; i < shown.size(); ++i) {
            INFO("row " << i);
            CHECK(row_name(shown[i]) == expected[i]);
            // The flat match list shows the vendor beside the model.
            lv_obj_t* vendor_label = lv_obj_get_child(shown[i], 1);
            REQUIRE(vendor_label != nullptr);
            CHECK_FALSE(lv_obj_has_flag(vendor_label, LV_OBJ_FLAG_HIDDEN));
        }
    }

    SECTION("query matching a vendor keeps every machine it makes") {
        lv_textarea_set_text(input, "flashforge");

        size_t expected = 0;
        for (const auto& e : entries) {
            if (helix::ui::selector_entry_matches(e, "flashforge")) {
                ++expected;
            }
        }
        REQUIRE(expected > 1);

        CHECK(view() == kViewSearch);
        CHECK(visible_rows().size() == expected);
    }

    SECTION("query matching only a vendor string, not any model name, still hits") {
        // No bundled model name contains "ants"; only the PrintersForAnts
        // bucket does, so this pins the vendor arm of the match.
        lv_textarea_set_text(input, "ants");

        size_t expected = 0;
        for (const auto& e : entries) {
            if (e.group == "PrintersForAnts") {
                ++expected;
            }
        }
        REQUIRE(expected >= 1);

        CHECK(view() == kViewSearch);
        CHECK(visible_rows().size() == expected);
    }

    SECTION("query matching nothing shows the empty state") {
        lv_textarea_set_text(input, "zzz-no-such-machine");

        lv_subject_t* matches = lv_xml_get_subject(nullptr, "wizard_printer_match_count");
        REQUIRE(matches != nullptr);
        CHECK(lv_subject_get_int(matches) == 0);
        CHECK(visible_rows().empty());
    }
}

TEST_CASE_METHOD(WizardPrinterIdentifyUIFixture,
                 "Wizard printer search clearing returns to the tile grid", "[wizard][selector]") {
    require_ready();

    lv_obj_t* input = search_input();
    REQUIRE(input != nullptr);

    lv_textarea_set_text(input, "k1");
    REQUIRE(view() == kViewSearch);
    REQUIRE_FALSE(visible_rows().empty());

    lv_textarea_set_text(input, "");
    CHECK(view() == kViewTiles);
    CHECK_FALSE(lv_obj_has_flag(tiles_container(), LV_OBJ_FLAG_HIDDEN));
}

// ============================================================================
// Vendor drill-in
// ============================================================================

TEST_CASE_METHOD(WizardPrinterIdentifyUIFixture,
                 "Wizard vendor tiles hold the vendor name to one line",
                 "[wizard][selector][one_line]") {
    require_ready();
    lv_textarea_set_text(search_input(), "");
    REQUIRE(view() == kViewTiles);

    lv_obj_t* tile = tile_for("Anycubic");
    REQUIRE(tile != nullptr);
    lv_obj_t* name = helix::test::find_dotted_label(tile);
    REQUIRE(name != nullptr);
    CHECK(helix::test::held_to_one_line(name));
}

TEST_CASE_METHOD(WizardPrinterIdentifyUIFixture, "Wizard vendor tile drills into its models",
                 "[wizard][selector]") {
    require_ready();

    // Enter the browse level deterministically: an empty query always lands
    // there, whatever the auto-detection picked.
    lv_textarea_set_text(search_input(), "");
    REQUIRE(view() == kViewTiles);

    const auto entries = detector_entries();
    const std::string vendor = "Anycubic";
    lv_obj_t* tile = tile_for(vendor);
    REQUIRE(tile != nullptr);

    size_t expected = 0;
    for (const auto& e : entries) {
        if (e.group == vendor) {
            ++expected;
        }
    }
    REQUIRE(expected > 1);

    lv_obj_send_event(tile, LV_EVENT_CLICKED, nullptr);

    REQUIRE(view() == kViewVendor);
    const auto shown = visible_rows();
    CHECK(shown.size() == expected);
    for (lv_obj_t* row : shown) {
        const std::string name = row_name(row);
        const auto it = std::find_if(entries.begin(), entries.end(),
                                     [&](const auto& e) { return e.label == name; });
        INFO(name);
        REQUIRE(it != entries.end());
        CHECK(it->group == vendor);
    }

    // The back affordance returns to the tile grid.
    lv_obj_t* back = lv_obj_find_by_name(step_root, "vendor_back_btn");
    REQUIRE(back != nullptr);
    lv_obj_send_event(back, LV_EVENT_CLICKED, nullptr);
    CHECK(view() == kViewTiles);
}

// ============================================================================
// Prior selection: auto-drill through the revisit (cached) path
// ============================================================================

TEST_CASE_METHOD(WizardPrinterIdentifyUIFixture,
                 "Wizard re-entry opens the selected machine's vendor at the machine",
                 "[wizard][selector]") {
    require_ready();

    const std::string machine = "FlashForge Adventurer 5M";

    // Select the machine the way a user does: search it, click its row.
    lv_textarea_set_text(search_input(), "Adventurer 5M");
    REQUIRE(view() == kViewSearch);

    lv_obj_t* target = nullptr;
    for (lv_obj_t* row : visible_rows()) {
        if (row_name(row) == machine) {
            target = row;
            break;
        }
    }
    REQUIRE(target != nullptr);
    lv_obj_send_event(target, LV_EVENT_CLICKED, nullptr);

    lv_subject_t* selected = lv_xml_get_subject(nullptr, "printer_type_selected");
    REQUIRE(selected != nullptr);
    const int index = lv_subject_get_int(selected);
    REQUIRE(PrinterDetector::get_list_name_at(index, "") == machine);

    // Leave and re-enter the step: cleanup() caches the rows, create() must
    // restore them AND land inside the machine's vendor bucket.
    auto* step = get_wizard_printer_identify_step();
    step->cleanup();
    // Screen-sized, as the wizard content area is: the vendor title holds one
    // dotted line and would ellipsize in a default-sized box.
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_set_size(parent, lv_pct(100), lv_pct(100));
    REQUIRE(step->create(parent) != nullptr);
    step_root = parent;

    REQUIRE(view() == kViewVendor);

    const auto entries = detector_entries();
    size_t expected = 0;
    for (const auto& e : entries) {
        if (e.group == "FlashForge") {
            ++expected;
        }
    }
    REQUIRE(expected > 1);
    CHECK(visible_rows().size() == expected);

    lv_obj_t* title = lv_obj_find_by_name(step_root, "vendor_title_label");
    REQUIRE(title != nullptr);
    CHECK(lv_streq(lv_label_get_text(title), "FlashForge"));

    // The selected machine's row survived the cache round-trip and is showing.
    bool found = false;
    for (lv_obj_t* row : visible_rows()) {
        found = found || (row_name(row) == machine);
    }
    CHECK(found);

    step->cleanup();
}

// ============================================================================
// Singleton buckets: pseudo-machines must stay reachable and selectable
// ============================================================================

TEST_CASE_METHOD(WizardPrinterIdentifyUIFixture, "Wizard singleton tiles drill into their own row",
                 "[wizard][selector]") {
    require_ready();

    // Enter the browse level deterministically.
    lv_textarea_set_text(search_input(), "");
    REQUIRE(view() == kViewTiles);

    for (const std::string bucket : {"Custom/Other", "Unknown"}) {
        CAPTURE(bucket);
        lv_obj_t* tile = tile_for(bucket);
        REQUIRE(tile != nullptr);
        lv_obj_send_event(tile, LV_EVENT_CLICKED, nullptr);

        // A singleton bucket shows exactly its own row.
        REQUIRE(view() == kViewVendor);
        const auto shown = visible_rows();
        REQUIRE(shown.size() == 1);
        CHECK(row_name(shown[0]) == bucket);

        // ...and the row is selectable: an unsupported printer still picks
        // Custom/Other from its own tile.
        lv_obj_send_event(shown[0], LV_EVENT_CLICKED, nullptr);
        lv_subject_t* selected = lv_xml_get_subject(nullptr, "printer_type_selected");
        REQUIRE(selected != nullptr);
        CHECK(PrinterDetector::get_list_name_at(lv_subject_get_int(selected), "") == bucket);

        // Back out before the next bucket.
        lv_obj_t* back = lv_obj_find_by_name(step_root, "vendor_back_btn");
        REQUIRE(back != nullptr);
        lv_obj_send_event(back, LV_EVENT_CLICKED, nullptr);
        REQUIRE(view() == kViewTiles);
    }
}

// ============================================================================
// Real-database grouping completeness
// ============================================================================

TEST_CASE("Wizard selector groups every visible machine exactly once", "[selector][wizard]") {
    const auto entries = detector_entries();
    REQUIRE(entries.size() > 50);

    const auto groups = helix::ui::group_selector_entries(entries);

    std::vector<std::string> seen;
    size_t total = 0;
    for (const auto& g : groups) {
        REQUIRE_FALSE(g.entries.empty());
        for (const auto* e : g.entries) {
            seen.push_back(e->label);
            ++total;
        }
    }
    CHECK(total == entries.size());

    std::sort(seen.begin(), seen.end());
    const auto duplicate = std::adjacent_find(seen.begin(), seen.end());
    CHECK(duplicate == seen.end());

    // The pseudo-machines stay reachable as their own buckets.
    const auto bucket_of = [&](const std::string& label) {
        for (const auto& g : groups) {
            for (const auto* e : g.entries) {
                if (e->label == label) {
                    return g.name;
                }
            }
        }
        return std::string();
    };
    CHECK(bucket_of("Custom/Other") == "Custom/Other");
    CHECK(bucket_of("Unknown") == "Unknown");
}

// ============================================================================
// Ambiguous detection hands the choice to the user (#1606, #1607)
// ============================================================================

namespace {

// A K2 whose hostname names neither model and whose config declares no bed:
// the detector cannot tell the K2 Plus from the K2 Pro.
class WizardPrinterIdentifyTiedFixture : public WizardPrinterIdentifyUIFixture {
  public:
    WizardPrinterIdentifyTiedFixture() : WizardPrinterIdentifyUIFixture(DeferOpen{}) {
        Config* config = Config::get_instance();
        saved_type_ = config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");
        saved_name_ = config->get<std::string>(config->df() + helix::wizard::PRINTER_NAME, "");

        const std::vector<std::string> objects = {"extruder",
                                                  "heater_bed",
                                                  "box",
                                                  "motor_control",
                                                  "fan_feedback",
                                                  "load_ai",
                                                  "filament_rack",
                                                  "heater_generic chamber_heater",
                                                  "temperature_sensor chamber_temp"};
        helix::PrinterDiscovery& hardware = api()->hardware();
        hardware.parse_objects(nlohmann::json(objects));
        hardware.set_printer_objects(objects);
        hardware.set_hostname("creality-k2");
        hardware.parse_config_keys(nlohmann::json{{"printer", {{"kinematics", "corexy"}}}});
        set_moonraker_api(api());

        // The step detects once per printer; make it detect this one.
        WizardPrinterIdentifyStepTestAccess::forget_printer(*get_wizard_printer_identify_step());
        open();
    }

    ~WizardPrinterIdentifyTiedFixture() {
        close_step();
        set_moonraker_api(nullptr);
        // The next fixture opens without a printer and must list every machine.
        WizardPrinterIdentifyStepTestAccess::forget_printer(*get_wizard_printer_identify_step());
        Config* config = Config::get_instance();
        config->set<std::string>(config->df() + helix::wizard::PRINTER_TYPE, saved_type_);
        config->set<std::string>(config->df() + helix::wizard::PRINTER_NAME, saved_name_);
    }

    // Leaves the step once, the way Back or Next does.
    void close_step() {
        if (ready_) {
            get_wizard_printer_identify_step()->cleanup();
            ready_ = false;
        }
    }

    int selected_index() {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, "printer_type_selected");
        REQUIRE(subject != nullptr);
        return lv_subject_get_int(subject);
    }

    bool next_enabled() {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, "connection_test_passed");
        REQUIRE(subject != nullptr);
        return lv_subject_get_int(subject) == 1;
    }

    std::string saved_type_;
    std::string saved_name_;
};

} // namespace

TEST_CASE_METHOD(WizardPrinterIdentifyTiedFixture,
                 "Wizard opens a tied detection on its candidates with nothing selected",
                 "[wizard][selector][1606]") {
    require_ready();

    // The step reads this discovery, and the detector calls it a tie.
    const PrinterDetectionResult detection = PrinterDetector::auto_detect(api()->hardware());
    CAPTURE(detection.type_name, detection.runner_up_type_name, detection.margin());
    REQUIRE(detection.ambiguous());

    SECTION("the tied candidates are listed and nothing is selected") {
        CHECK(view() == kViewCandidates);
        std::vector<std::string> shown;
        for (lv_obj_t* row : visible_rows()) {
            shown.push_back(row_name(row));
        }
        // Rows keep the printer list's own order.
        CHECK(shown ==
              std::vector<std::string>{"Creality K2", "Creality K2 Plus", "Creality K2 Pro"});
        CHECK(selected_index() < 0);
        CHECK_FALSE(next_enabled());

        // The header says why the list is short, and the status names no
        // machine as the detected one.
        lv_obj_t* candidates_title = lv_obj_find_by_name(step_root, "candidates_title_label");
        lv_obj_t* vendor_title = lv_obj_find_by_name(step_root, "vendor_title_label");
        REQUIRE(candidates_title != nullptr);
        REQUIRE(vendor_title != nullptr);
        CHECK_FALSE(lv_obj_has_flag(lv_obj_get_parent(candidates_title), LV_OBJ_FLAG_HIDDEN));
        CHECK_FALSE(lv_obj_has_flag(candidates_title, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_has_flag(vendor_title, LV_OBJ_FLAG_HIDDEN));
        CHECK(std::string(get_wizard_printer_identify_step()->get_detection_status()) ==
              lv_tr("Several printers match - choose yours"));
    }

    SECTION("leaving without a pick persists no printer type") {
        close_step();
        Config* config = Config::get_instance();
        // cleanup() ran: it saved the name it filled in from the hostname.
        CHECK(config->get<std::string>(config->df() + helix::wizard::PRINTER_NAME, "") ==
              "creality-k2");
        CHECK(config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "").empty());
    }

    SECTION("picking a candidate selects it and enables Next") {
        lv_obj_t* pro = nullptr;
        for (lv_obj_t* row : visible_rows()) {
            if (row_name(row) == "Creality K2 Pro") {
                pro = row;
            }
        }
        REQUIRE(pro != nullptr);
        lv_obj_send_event(pro, LV_EVENT_CLICKED, nullptr);
        CHECK(PrinterDetector::get_list_name_at(selected_index(), "corexy") == "Creality K2 Pro");
        CHECK(next_enabled());
        // Leave without a printer so cleanup() only records the pick; applying
        // the K2 preset is the detector tests' business.
        set_moonraker_api(nullptr);
    }
}
