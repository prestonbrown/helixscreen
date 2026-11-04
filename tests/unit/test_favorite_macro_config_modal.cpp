// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: instantiates the shipped ui_xml/favorite_macro_config_modal.xml through
//                 lv_xml_create() at run time and asserts on the real widget tree.
//                 ../test_fixtures.h pulls in include/printer_state.h,
//                 include/moonraker_api.h and include/theme_manager.h.
#include "ui_modal.h"
#include "ui_swatch.h"

#include "../test_fixtures.h"
#include "app_globals.h"
#include "config.h"
#include "core/static_subject_registry.h"
#include "core/update_queue.h"
#include "favorite_macro_config_modal.h"
#include "favorite_macro_widget.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "macro_param_cache.h"
#include "macro_param_defaults.h"
#include "macro_param_modal.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"

#include <map>
#include <memory>

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

TEST_CASE_METHOD(FavoriteMacroRunFixture,
                 "favorite macro widget with confirmation off sends an UNKNOWN macro's saved text",
                 "[macro][favorite_config][wiring]") {
    // Registered with Klipper, absent from configfile: its parameters are unknown.
    helix::MacroParamCache::instance().populate_from_configfile(nlohmann::json::object(),
                                                                {"MY_MACRO"});
    REQUIRE(helix::MacroParamCache::instance().get("MY_MACRO").knowledge ==
            helix::MacroParamKnowledge::UNKNOWN);

    helix::MacroParamDefaultRecord record;
    record.values = {{"PURGE", "1"}};
    record.ask_for_params = false;
    helix::MacroParamDefaults::instance().set("MY_MACRO", record);

    attach_widget(/*require_confirmation=*/false);
    widget.handle_clicked();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(widget_script_sent_containing(mock_client, "MY_MACRO PURGE=1"));
    helix::MacroParamDefaults::instance().clear("MY_MACRO");
}

TEST_CASE_METHOD(FavoriteMacroRunFixture,
                 "favorite macro widget asking for an UNKNOWN macro prefills the free-form text",
                 "[macro][favorite_config][wiring]") {
    helix::MacroParamCache::instance().populate_from_configfile(nlohmann::json::object(),
                                                                {"MY_MACRO"});

    helix::MacroParamDefaultRecord record;
    record.values = {{"PURGE", "1"}};
    helix::MacroParamDefaults::instance().set("MY_MACRO", record);

    attach_widget(/*require_confirmation=*/true);
    widget.handle_clicked();
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(widget_script_sent_containing(mock_client, "MY_MACRO"));
    lv_obj_t* prompt = lv_obj_find_by_name(lv_screen_active(), "macro_param_modal");
    REQUIRE(prompt != nullptr);
    lv_obj_t* field = lv_obj_find_by_name(prompt, "field_input");
    REQUIRE(field != nullptr);
    CHECK(std::string(lv_textarea_get_text(field)) == "PURGE=1");
    helix::MacroParamModal::cancel_cb(nullptr);
    helix::ui::UpdateQueue::instance().drain();
    helix::MacroParamDefaults::instance().clear("MY_MACRO");
}

// =============================================================================
// Options tab: the "Default Parameters" entry and its editor
// =============================================================================

namespace {

/// The C++ config modal over the shipped XML, pointed at a widget whose config
/// lives in an ad-hoc panel. PanelWidgetManager creates per-panel config on
/// demand, so no panel needs to exist.
class ConfigModalDefaultsFixture : public XMLTestFixture {
  public:
    ConfigModalDefaultsFixture() {
        helix::MacroParamCache::instance().clear();
    }
    ~ConfigModalDefaultsFixture() override {
        ModalStack::instance().clear();
        helix::MacroParamCache::instance().clear();
    }

    /// populate_from_configfile() clears the cache on every call, so every
    /// macro the case needs goes in through ONE call.
    /// @p unknown names are registered with Klipper but absent from configfile.
    static void cache_macros(std::initializer_list<std::pair<const char*, const char*>> macros,
                             std::initializer_list<const char*> unknown = {}) {
        nlohmann::json cfg;
        std::vector<std::string> names(unknown.begin(), unknown.end());
        for (const auto& [name, gcode] : macros) {
            cfg[std::string("gcode_macro ") + name]["gcode"] = gcode;
            names.emplace_back(name);
        }
        std::unordered_set<std::string> known(names.begin(), names.end());
        helix::MacroParamCache::instance().populate_from_configfile(cfg, known);
    }

    helix::FavoriteMacroConfigModal& show_config_for(const char* macro) {
        // Seed the layout Config the modal actually reads: PanelWidgetConfig
        // drops config for widget ids absent from a loaded page, so a bare
        // set_widget_config() is a silent no-op. "favorite_macro:1" is a
        // registry id (multi-instance), so parse_widget_array keeps the entry.
        nlohmann::json widget = {
            {"id", "favorite_macro:1"},
            {"enabled", true},
            {"config",
             {{"macro", macro}, {"icon", ""}, {"color", 0}, {"require_confirmation", true}}}};
        nlohmann::json layout;
        layout["pages"].push_back({{"id", "main"}, {"widgets", nlohmann::json::array({widget})}});
        auto* cfg = helix::Config::get_instance();
        cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/defaults_test_panel", layout);

        auto& wc = helix::PanelWidgetManager::instance().get_widget_config("defaults_test_panel");
        wc.mark_dirty();
        wc.load();
        REQUIRE(wc.get_widget_config("favorite_macro:1").value("macro", "") == macro);

        modal_ = std::make_unique<helix::FavoriteMacroConfigModal>("favorite_macro:1",
                                                                   "defaults_test_panel");
        modal_->show(lv_screen_active());
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(modal_->dialog() != nullptr);
        return *modal_;
    }

    std::unique_ptr<helix::FavoriteMacroConfigModal> modal_;
};

} // namespace

TEST_CASE_METHOD(ConfigModalDefaultsFixture,
                 "Options shows Default Parameters only for macros that take parameters",
                 "[macro][favorite_config][defaults]") {
    cache_macros({{"MY_MACRO", "G1 E{params.TEMP|default(10)}"}, {"PLAIN_MACRO", "G28"}},
                 {"PY_LOAD"});

    // The visibility bind lives on the wrapper (a hidden flex child drops out of
    // layout); the row itself never carries the flag.
    auto& with_params = show_config_for("MY_MACRO");
    lv_obj_t* wrap = lv_obj_find_by_name(with_params.dialog(), "default_params_wrap");
    REQUIRE(wrap != nullptr);
    REQUIRE(lv_obj_find_by_name(with_params.dialog(), "row_default_params") != nullptr);
    CHECK_FALSE(lv_obj_has_flag(wrap, LV_OBJ_FLAG_HIDDEN));

    with_params.hide();
    helix::ui::UpdateQueue::instance().drain();

    auto& without_params = show_config_for("PLAIN_MACRO");
    lv_obj_t* plain_wrap = lv_obj_find_by_name(without_params.dialog(), "default_params_wrap");
    REQUIRE(plain_wrap != nullptr);
    CHECK(lv_obj_has_flag(plain_wrap, LV_OBJ_FLAG_HIDDEN));

    without_params.hide();
    helix::ui::UpdateQueue::instance().drain();

    auto& unknown_params = show_config_for("PY_LOAD");
    lv_obj_t* unknown_wrap = lv_obj_find_by_name(unknown_params.dialog(), "default_params_wrap");
    REQUIRE(unknown_wrap != nullptr);
    CHECK_FALSE(lv_obj_has_flag(unknown_wrap, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(ConfigModalDefaultsFixture,
                 "Default Parameters for an UNKNOWN macro saves its free-form text",
                 "[macro][favorite_config][defaults]") {
    cache_macros({}, {"PY_LOAD"});

    show_config_for("PY_LOAD");
    helix::FavoriteMacroConfigModal::defaults_cb(nullptr);
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* editor = lv_obj_find_by_name(lv_screen_active(), "macro_param_modal");
    REQUIRE(editor != nullptr);
    REQUIRE_FALSE(
        lv_obj_has_flag(lv_obj_find_by_name(editor, "modal_title_save"), LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* field = lv_obj_find_by_name(editor, "field_input");
    REQUIRE(field != nullptr);
    lv_textarea_set_text(field, "purge=1");

    helix::MacroParamModal::save_cb(nullptr);
    helix::ui::UpdateQueue::instance().drain();

    const helix::MacroParamDefaultRecord saved =
        helix::MacroParamDefaults::instance().get("PY_LOAD");
    CHECK(saved.values == std::map<std::string, std::string>{{"PURGE", "1"}});
    CHECK(saved.ask_for_params);
    helix::MacroParamDefaults::instance().clear("PY_LOAD");
}

TEST_CASE_METHOD(ConfigModalDefaultsFixture,
                 "Default Parameters opens the editor and Save persists the record",
                 "[macro][favorite_config][defaults]") {
    cache_macros({{"MY_MACRO", "G1 E{params.TEMP|default(10)}"}});

    // An existing record prefills the editor and starts the toggle ON.
    helix::MacroParamDefaultRecord existing;
    existing.values = {{"TEMP", "200"}};
    helix::MacroParamDefaults::instance().set("MY_MACRO", existing);

    show_config_for("MY_MACRO");
    helix::FavoriteMacroConfigModal::defaults_cb(nullptr);
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* editor = lv_obj_find_by_name(lv_screen_active(), "macro_param_modal");
    REQUIRE(editor != nullptr);
    REQUIRE_FALSE(
        lv_obj_has_flag(lv_obj_find_by_name(editor, "modal_title_save"), LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* field = lv_obj_find_by_name(editor, "field_input");
    REQUIRE(field != nullptr);
    REQUIRE(std::string(lv_textarea_get_text(field)) == "200");

    lv_textarea_set_text(field, "215");
    lv_obj_t* ask = lv_obj_find_by_name(lv_obj_find_by_name(editor, "ask_row"), "toggle");
    REQUIRE(lv_obj_has_state(ask, LV_STATE_CHECKED));
    lv_obj_remove_state(ask, LV_STATE_CHECKED);
    lv_obj_send_event(ask, LV_EVENT_VALUE_CHANGED, nullptr);

    helix::MacroParamModal::save_cb(nullptr);
    helix::ui::UpdateQueue::instance().drain();

    const helix::MacroParamDefaultRecord saved =
        helix::MacroParamDefaults::instance().get("MY_MACRO");
    CHECK(saved.values == std::map<std::string, std::string>{{"TEMP", "215"}});
    CHECK_FALSE(saved.ask_for_params);
}

TEST_CASE_METHOD(ConfigModalDefaultsFixture,
                 "Appearance colours are painter swatches; a tap rings and saves one",
                 "[macro][favorite_config][swatch]") {
    auto& modal = show_config_for("PLAIN_MACRO");
    lv_obj_t* dlg = modal.dialog();

    lv_obj_t* red = lv_obj_find_by_name(dlg, "tile_color_swatch_0");
    lv_obj_t* theme_default = lv_obj_find_by_name(dlg, "tile_color_swatch_15");
    REQUIRE(red != nullptr);
    REQUIRE(theme_default != nullptr);
    CHECK(lv_obj_find_by_name(dlg, "tile_color_swatch_16") == nullptr);
    for (lv_obj_t* swatch : {red, theme_default}) {
        lv_obj_t* disc = lv_obj_find_by_name(swatch, "color_swatch_disc");
        REQUIRE(disc != nullptr);
        CHECK(helix::ui::has_swatch_paint(disc));
        CHECK(lv_obj_get_style_radius(disc, LV_PART_MAIN) == LV_RADIUS_CIRCLE);
    }
    CHECK((lv_color_to_u32(helix::ui::swatch_color_of(red)) & 0xFFFFFF) == 0xE53935);
    CHECK(lv_color_eq(helix::ui::swatch_color_of(theme_default),
                      theme_manager_get_color("secondary")));

    // The saved colour 0 is the theme default, the last swatch.
    lv_subject_t* selected = lv_xml_get_subject(nullptr, "tile_color_selected");
    REQUIRE(selected != nullptr);
    CHECK(lv_subject_get_int(selected) == 15);

    lv_obj_t* amber = lv_obj_find_by_name(dlg, "tile_color_swatch_3");
    REQUIRE(amber != nullptr);
    lv_obj_send_event(amber, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(lv_subject_get_int(selected) == 3);
    auto& wc = helix::PanelWidgetManager::instance().get_widget_config("defaults_test_panel");
    CHECK(wc.get_widget_config("favorite_macro:1").value("color", 0u) == 0xFFC107u);
}

TEST_CASE_METHOD(ConfigModalDefaultsFixture,
                 "the config modal opens and binds after a printer switch",
                 "[macro][favorite_config][subjects]") {
    // Its subjects are registered once, with the widget factories, and nothing on a
    // printer switch's rebuild path registers them again.
    helix::register_favorite_macro_config_subjects();
    cache_macros({{"PLAIN_MACRO", "G28"}});
    REQUIRE(StaticSubjectRegistry::instance().deinit_one(
        "FavoriteMacroConfigSubjects", StaticSubjectRegistry::Scope::PrinterSwitch));
    REQUIRE(StaticSubjectRegistry::instance().deinit_one(
        "TileColorSubjects", StaticSubjectRegistry::Scope::PrinterSwitch));

    REQUIRE(lv_xml_get_subject(nullptr, "tile_color_swatch_color_0") != nullptr);
    lv_subject_t* tab = lv_xml_get_subject(nullptr, "fav_macro_config_tab");
    REQUIRE(tab != nullptr);

    auto& modal = show_config_for("PLAIN_MACRO");
    lv_obj_t* sec_macro = lv_obj_find_by_name(modal.dialog(), "section_macro");
    lv_obj_t* sec_options = lv_obj_find_by_name(modal.dialog(), "section_options");
    REQUIRE(sec_macro != nullptr);
    REQUIRE(sec_options != nullptr);

    lv_subject_set_int(tab, 2);
    lv_obj_update_layout(modal.dialog());
    CHECK(lv_obj_has_flag(sec_macro, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(sec_options, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(ConfigModalDefaultsFixture, "a tap on a macro row selects and saves that macro",
                 "[macro][favorite_config]") {
    cache_macros({{"PLAIN_MACRO", "G28"}, {"OTHER_MACRO", "G1 X10"}});
    api().hardware().parse_objects(
        nlohmann::json{"gcode_macro PLAIN_MACRO", "gcode_macro OTHER_MACRO"});
    struct ApiScope {
        IMoonrakerAPI* previous = get_moonraker_api();
        ~ApiScope() {
            set_moonraker_api(previous);
        }
    } api_scope;
    set_moonraker_api(&api());
    auto& modal = show_config_for("PLAIN_MACRO");
    lv_obj_t* list = lv_obj_find_by_name(modal.dialog(), "macro_list");
    REQUIRE(list != nullptr);

    lv_obj_t* other = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(list); ++i) {
        lv_obj_t* row = lv_obj_get_child(list, i);
        const auto* name = static_cast<const std::string*>(lv_obj_get_user_data(row));
        if (name && *name == "OTHER_MACRO")
            other = row;
    }
    REQUIRE(other != nullptr);
    lv_obj_send_event(other, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    auto& wc = helix::PanelWidgetManager::instance().get_widget_config("defaults_test_panel");
    CHECK(wc.get_widget_config("favorite_macro:1").value("macro", "") == "OTHER_MACRO");
}
