// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: instantiates the shipped ui_xml/favorite_macro_config_modal.xml through
//                 lv_xml_create() at run time and asserts on the real widget tree.
//                 ../test_fixtures.h pulls in include/printer_state.h,
//                 include/moonraker_api.h and include/theme_manager.h.
#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "app_globals.h"
#include "favorite_macro_widget.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "macro_param_cache.h"
#include "macro_param_defaults.h"
#include "moonraker_client_mock.h"

#include "../catch_amalgamated.hpp"

// Verifies the new XML component instantiates and tab sections toggle on the subject.
TEST_CASE_METHOD(XMLTestFixture, "favorite_macro_config_modal instantiates + tabs switch",
                 "[macro][favorite_config][xml]") {
    // Component + subjects must be registered by register_favorite_macro_widgets()
    lv_obj_t* dlg = static_cast<lv_obj_t*>(
        lv_xml_create(lv_screen_active(), "favorite_macro_config_modal", nullptr));
    REQUIRE(dlg != nullptr);

    lv_obj_t* sec_macro = lv_obj_find_by_name(dlg, "section_macro");
    lv_obj_t* sec_appearance = lv_obj_find_by_name(dlg, "section_appearance");
    lv_obj_t* sec_options = lv_obj_find_by_name(dlg, "section_options");
    REQUIRE(sec_macro != nullptr);
    REQUIRE(sec_appearance != nullptr);
    REQUIRE(sec_options != nullptr);

    lv_subject_t* tab = lv_xml_get_subject(nullptr, "fav_macro_config_tab");
    REQUIRE(tab != nullptr);

    lv_subject_set_int(tab, 0);
    lv_obj_update_layout(dlg);
    REQUIRE_FALSE(lv_obj_has_flag(sec_macro, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_has_flag(sec_options, LV_OBJ_FLAG_HIDDEN));

    lv_subject_set_int(tab, 2);
    lv_obj_update_layout(dlg);
    REQUIRE(lv_obj_has_flag(sec_macro, LV_OBJ_FLAG_HIDDEN));
    REQUIRE_FALSE(lv_obj_has_flag(sec_options, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(dlg);
}

// =============================================================================
// Saved parameter defaults on the widget's run path
// =============================================================================

namespace {

/// True when the mock client ran a script containing @p needle.
bool widget_script_sent_containing(const MoonrakerClientMock& client, const std::string& needle) {
    for (const auto& script : client.gcode_script_history()) {
        if (script.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// A FavoriteMacroWidget attached to a bare host object, running against a mock
/// API. attach() tolerates missing named children (it warns and renders blank),
/// so no widget XML is needed to reach the run path.
class FavoriteMacroRunFixture : public XMLTestFixture {
  public:
    MoonrakerClientMock mock_client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI mock_api{mock_client, state()};
    IMoonrakerAPI* previous_api_ = nullptr;

    FavoriteMacroWidget widget{"run_test_widget"};
    lv_obj_t* host = nullptr;

    FavoriteMacroRunFixture() {
        previous_api_ = get_moonraker_api();
        set_moonraker_api(&mock_api);
        state().init_subjects(false);
        state().set_klippy_state_sync(helix::KlippyState::READY);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        helix::MacroParamCache::instance().clear();
    }

    ~FavoriteMacroRunFixture() override {
        set_moonraker_api(previous_api_);
        ModalStack::instance().clear();
        helix::MacroParamCache::instance().clear();
        mock_client.disconnect();
    }

    void attach_widget(bool require_confirmation) {
        widget.set_config({{"macro", "MY_MACRO"},
                           {"icon", ""},
                           {"color", 0},
                           {"require_confirmation", require_confirmation}});
        host = lv_obj_create(lv_screen_active());
        widget.attach(host, lv_screen_active());
    }
};

} // namespace

TEST_CASE_METHOD(FavoriteMacroRunFixture,
                 "favorite macro widget with confirmation off sends saved values",
                 "[macro][favorite_config][wiring]") {
    // MY_MACRO declares TEMP; the saved record says never ask, TEMP=210.
    nlohmann::json cfg;
    cfg["gcode_macro MY_MACRO"]["gcode"] = "{% set TEMP = params.TEMP|default(200)|int %}\nG1 E10";
    helix::MacroParamCache::instance().populate_from_configfile(cfg, {"MY_MACRO"});

    helix::MacroParamDefaultRecord record;
    record.values = {{"TEMP", "210"}};
    record.ask_for_params = false;
    helix::MacroParamDefaults::instance().set("MY_MACRO", record);

    attach_widget(/*require_confirmation=*/false);
    widget.handle_clicked();
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(widget_script_sent_containing(mock_client, "MY_MACRO"));
    CHECK(widget_script_sent_containing(mock_client, "MY_MACRO TEMP=210"));
}

TEST_CASE_METHOD(FavoriteMacroRunFixture,
                 "favorite macro widget with confirmation off runs bare without a record",
                 "[macro][favorite_config][wiring]") {
    // No saved record: the widget's confirmation opt-out keeps today's shape -
    // one tap, no modal, no parameters. The record is what adds the values.
    nlohmann::json cfg;
    cfg["gcode_macro MY_MACRO"]["gcode"] = "{% set TEMP = params.TEMP|default(200)|int %}\nG1 E10";
    helix::MacroParamCache::instance().populate_from_configfile(cfg, {"MY_MACRO"});

    attach_widget(/*require_confirmation=*/false);
    widget.handle_clicked();
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(widget_script_sent_containing(mock_client, "MY_MACRO"));
    CHECK_FALSE(widget_script_sent_containing(mock_client, "MY_MACRO TEMP"));
}
