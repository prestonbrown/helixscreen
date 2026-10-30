// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Text that C++ formats with lv_tr() outside the home widgets: the printer
// layer's discovered names and health texts, the filament panel and the
// print-select file metadata, each re-rendered on a live language switch.

#include "ui_panel_filament.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_state.h"
#include "config.h"
#include "format_utils.h"
#include "hardware_validator.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "i18n_plural.h"
#include "lvgl.h"
#include "printer_state.h"
#include "system_settings_manager.h"
#include "tool_state.h"

#include <spdlog/fmt/fmt.h>

#include <cstdio>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

void drain_all() {
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

std::string subject_text(lv_subject_t* subject) {
    const char* v = lv_subject_get_string(subject);
    return v ? v : "";
}

struct RussianFixture : public LVGLUITestFixture {
    ~RussianFixture() override {
        SystemSettingsManager::instance().set_language("en");
        drain_all();
    }
};

} // namespace

TEST_CASE_METHOD(RussianFixture, "Discovered fan names re-translate; a user's own name is kept",
                 "[i18n][fan]") {
    auto* config = Config::get_instance();
    const std::string key = config->df() + "fans/names/output_pin fan1";
    const std::string orig = config->get<std::string>(key, "");
    config->set(key, std::string("My Fan"));

    state().fan_state().init_fans({"fan", "output_pin fan1"});
    REQUIRE(state().fan_state().get_fans()[0].display_name == "Part Cooling Fan");
    REQUIRE(state().fan_state().get_fans()[1].display_name == "My Fan");
    const int version = lv_subject_get_int(state().fan_state().get_fans_version_subject());

    SystemSettingsManager::instance().set_language("ru");
    REQUIRE(std::string(lv_tr("Part Cooling Fan")) != "Part Cooling Fan");
    state().refresh_translated_texts();

    CHECK(state().fan_state().get_fans()[0].display_name == lv_tr("Part Cooling Fan"));
    CHECK(state().fan_state().get_fans()[1].display_name == "My Fan");
    CHECK(lv_subject_get_int(state().fan_state().get_fans_version_subject()) != version);

    config->set(key, orig);
}

TEST_CASE_METHOD(RussianFixture, "Hardware-health texts re-translate from the stored result",
                 "[i18n][hardware]") {
    HardwareValidationResult result;
    result.newly_discovered.push_back(
        HardwareIssue::info("fan_generic a", HardwareType::FAN, "Fan available"));
    result.newly_discovered.push_back(
        HardwareIssue::info("fan_generic b", HardwareType::FAN, "Fan available"));
    state().hardware_validation_state().set_hardware_validation_result(result);
    REQUIRE(subject_text(state().hardware_validation_state().get_hardware_issues_label_subject()) ==
            "2 Hardware Issues");

    SystemSettingsManager::instance().set_language("ru");
    REQUIRE(std::string(lv_tr("{} Hardware Issues")) != "{} Hardware Issues");
    state().refresh_translated_texts();

    CHECK(subject_text(state().hardware_validation_state().get_hardware_issues_label_subject()) ==
          fmt::format(lv_tr("{} Hardware Issues"), 2));
}

TEST_CASE_METHOD(RussianFixture, "FilamentPanel re-formats its status and safety texts",
                 "[i18n][filament]") {
    ToolState::instance().init_subjects(true);
    AmsState::instance().init_subjects(true);
    auto panel = std::make_unique<FilamentPanel>(state(), api());
    panel->init_subjects();
    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "filament_panel", nullptr));
    REQUIRE(root != nullptr);
    panel->setup(root, test_screen());
    drain_all();

    lv_subject_t* status = lv_xml_get_subject(nullptr, "filament_status");
    lv_subject_t* safety = lv_xml_get_subject(nullptr, "filament_safety_warning_text");
    REQUIRE(status != nullptr);
    REQUIRE(safety != nullptr);
    const std::string status_en = subject_text(status);
    const std::string safety_en = subject_text(safety);
    REQUIRE_FALSE(status_en.empty());
    REQUIRE(safety_en.rfind("Heat to at least ", 0) == 0);

    SystemSettingsManager::instance().set_language("ru");
    drain_all();

    REQUIRE(std::string(lv_tr(status_en.c_str())) != status_en);
    CHECK(subject_text(status) == lv_tr(status_en.c_str()));
    int min_temp = 0;
    REQUIRE(std::sscanf(safety_en.c_str(), "Heat to at least %d", &min_temp) == 1);
    char want[256];
    std::snprintf(want, sizeof(want), lv_tr("Heat to at least %d°C for filament operations"),
                  min_temp);
    REQUIRE(std::string(want) != safety_en);
    CHECK(subject_text(safety) == want); // whole, not cut off mid-character

    panel.reset();
    drain_all();
}

TEST_CASE_METHOD(PrintSelectPanelFixture,
                 "Print-select re-formats fetched file metadata; unfetched files stay blank",
                 "[i18n][print_select]") {
    PlantedGcode fetched("i18n_fetched.gcode");
    PlantedGcode pending("i18n_pending.gcode");
    panel_->refresh_files(true);
    drain();

    FileMetadata meta;
    meta.filename = fetched.name();
    meta.layer_count = 50;
    PrintSelectPanelTestAccess::apply_metadata(*panel_, fetched.name(), meta);
    drain();

    PrintSelectPanelTestAccess::forget_metadata(*panel_, pending.name());

    const PrintFileData* row = PrintSelectPanelTestAccess::find_file(*panel_, fetched.name());
    const PrintFileData* blank = PrintSelectPanelTestAccess::find_file(*panel_, pending.name());
    REQUIRE(row != nullptr);
    REQUIRE(blank != nullptr);
    REQUIRE(row->layer_count_str == "50 layers");
    REQUIRE_FALSE(blank->metadata_fetched);
    REQUIRE(blank->layer_count_str.empty());

    SystemSettingsManager::instance().set_language("ru");
    drain();

    char want[64];
    std::snprintf(want, sizeof(want), helix::ui::tr_plural("1 layer", "%u layers", 50u), 50u);
    REQUIRE(std::string(want) != "50 layers");
    row = PrintSelectPanelTestAccess::find_file(*panel_, fetched.name());
    blank = PrintSelectPanelTestAccess::find_file(*panel_, pending.name());
    CHECK(row->layer_count_str == want);
    CHECK(row->layer_count_str == "50 слоёв");
    CHECK(blank->layer_count_str.empty());

    SystemSettingsManager::instance().set_language("en");
    drain();
}

TEST_CASE_METHOD(RussianFixture, "Durations are formatted in the current language",
                 "[i18n][format_utils]") {
    REQUIRE(helix::format::duration_from_minutes(65) == "1h 5m");

    SystemSettingsManager::instance().set_language("ru");
    char want[64];
    std::snprintf(want, sizeof(want), lv_tr("%dh %dm"), 1, 5);
    REQUIRE(std::string(want) != "1h 5m");
    CHECK(helix::format::duration_from_minutes(65) == want);
}
