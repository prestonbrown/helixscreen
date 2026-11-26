// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mark on a material preset whose material runs a macro: it shows only
// then, it leaves every other name where it was, and it takes no press from
// the button it sits on.

#include "ui_modal.h"
#include "ui_panel_filament.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "../test_helpers/material_settings_scope.h"
#include "../test_helpers/scoped_geometry.h"
#include "../test_helpers/scoped_moonraker_api.h"
#include "../test_helpers/scoped_pointer_indev.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix/ui/text_metrics.h"
#include "material_settings_manager.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "preset_materials.h"
#include "printer_discovery.h"
#include "settings_manager.h"
#include "tool_state.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

using MaterialSettingsScope = helix::test::MaterialSettingsScope;
using Scripts = std::vector<std::string>;
using MaterialSettingsManager = helix::MaterialSettingsManager;

/// ABS with a bed temperature, so an override exists, and with @p macro when named.
filament::MaterialOverride abs_override(std::optional<std::string> macro) {
    filament::MaterialOverride result;
    result.nozzle_min = 250;
    result.nozzle_max = 250;
    result.bed_temp = 105;
    result.preheat_macro = std::move(macro);
    return result;
}

/// The Filament panel the way the app has it: the global one, built from its
/// markup over the global printer state and a mock printer, with a pointer to
/// press its buttons with. The panel is destroyed again on the way out.
struct MarkHarness {
    LVGLUITestFixture& fx;
    bool settings_live = (helix::SettingsManager::instance().init_subjects(), true);
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI api{client, get_printer_state()};
    ScopedMoonrakerApi installed{&api};
    helix_test::ScopedPointerIndev pointer;
    FilamentPanel& panel = get_global_filament_panel();
    lv_obj_t* root = nullptr;

    explicit MarkHarness(LVGLUITestFixture& f) : fx(f) {
        helix::ToolState::instance().init_subjects(true);
        helix::AmsState::instance().init_subjects(true);
        helix::AmsState::instance().clear_backends();
        state().set_klippy_state_sync(helix::KlippyState::READY);
        state().temperature_state().init_extruders({"extruder"});
        discover({"extruder", "heater_bed"});
        // The preset subjects exist before any markup binds to them, as in the app.
        helix::presets::init_subjects();
        panel.init_subjects();
        panel.set_api(&api);
        root = static_cast<lv_obj_t*>(lv_xml_create(fx.test_screen(), "filament_panel", nullptr));
        REQUIRE(root != nullptr);
        panel.setup(root, fx.test_screen());
        // A printer that is connected and ready leaves the panel's controls enabled.
        lv_subject_t* nav_enabled = lv_xml_get_subject(nullptr, "nav_buttons_enabled");
        REQUIRE(nav_enabled != nullptr);
        lv_subject_set_int(nav_enabled, 1);
        settle();
    }
    ~MarkHarness() {
        helix::ui::UpdateQueue::instance().drain();
        for (lv_obj_t* orphan : StaticPanelRegistry::instance().destroy_all()) {
            lv_obj_delete(orphan);
        }
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_delete(root);
        fx.process_lvgl(10);
    }
    MarkHarness(const MarkHarness&) = delete;
    MarkHarness& operator=(const MarkHarness&) = delete;

    static helix::PrinterState& state() {
        return get_printer_state();
    }

    /// What the printer says it has.
    static void discover(std::vector<std::string> objects) {
        helix::PrinterDiscovery hardware;
        hardware.parse_objects(nlohmann::json(objects));
        state().set_hardware(hardware);
        helix::ToolState::instance().init_tools(hardware);
    }

    void settle() {
        helix::ui::UpdateQueue::instance().drain();
        fx.process_lvgl(30);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// The first widget called @p name under @p from that is on screen.
    static lv_obj_t* shown_widget(lv_obj_t* from, std::string_view name) {
        if (lv_obj_is_hidden(from)) {
            return nullptr;
        }
        const char* own = lv_obj_get_name(from);
        if (own && name == own) {
            return from;
        }
        for (uint32_t i = 0; i < lv_obj_get_child_count(from); ++i) {
            if (lv_obj_t* found =
                    shown_widget(lv_obj_get_child(from, static_cast<int32_t>(i)), name)) {
                return found;
            }
        }
        return nullptr;
    }

    lv_obj_t* shown(const char* name) {
        lv_obj_t* found = shown_widget(root, name);
        REQUIRE(found != nullptr);
        return found;
    }

    lv_obj_t* under(lv_obj_t* parent, const char* name) {
        lv_obj_t* found = lv_obj_find_by_name(parent, name);
        REQUIRE(found != nullptr);
        return found;
    }

    /// Where @p preset's macro mark shows: before its name, before its
    /// temperature line, in both places or nowhere. The mark's widgets are
    /// <stem>_macro_mark and <stem>_macro_mark_temps.
    static std::string_view mark_place(lv_obj_t* preset, std::string_view stem) {
        const std::string by_name = std::string(stem) + "_macro_mark";
        const std::string by_temps = std::string(stem) + "_macro_mark_temps";
        lv_obj_t* name = lv_obj_find_by_name(preset, by_name.c_str());
        lv_obj_t* temps = lv_obj_find_by_name(preset, by_temps.c_str());
        REQUIRE(name != nullptr);
        REQUIRE(temps != nullptr);
        const bool before_name = !lv_obj_is_hidden(name);
        const bool before_temps = !lv_obj_is_hidden(temps);
        if (before_name && before_temps) {
            return "both";
        }
        return before_name ? "name" : before_temps ? "temps" : "none";
    }

    lv_area_t area(lv_obj_t* widget) {
        lv_obj_update_layout(root);
        lv_area_t bounds{};
        lv_obj_get_coords(widget, &bounds);
        return bounds;
    }

    static int center_x(const lv_area_t& bounds) {
        return (bounds.x1 + bounds.x2) / 2;
    }

    /// A finger held on @p preset's name until the hold fires, then lifted.
    /// The picker's dialog when one opened.
    lv_obj_t* hold(lv_obj_t* preset) {
        const lv_area_t name = area(under(preset, "preset_material_name"));
        const int x = center_x(name);
        const int y = (name.y1 + name.y2) / 2;
        pointer.press(x, y);
        pointer.hold(pointer.long_press_hold_ms());
        settle();
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        pointer.release(x, y);
        settle();
        return dialog;
    }
};

constexpr const char* kUnmarked[] = {"preset_pla", "preset_petg", "preset_tpu"};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A preset carries the macro mark only while its material names a macro",
                 "[filament_preset_mark][2183]") {
    // 0: no override. 1: an override whose macro is empty. 2: HEAT_ABS.
    const int state = GENERATE(0, 1, 2);
    CAPTURE(state);
    MaterialSettingsScope settings;
    MarkHarness m(*this);
    if (state > 0) {
        MaterialSettingsManager::instance().set_override(
            "ABS", abs_override(state == 2 ? std::optional<std::string>{"HEAT_ABS"}
                                           : std::optional<std::string>{""}));
    }
    helix::presets::refresh_subjects();
    m.settle();

    REQUIRE(helix::presets::name(2) == "ABS");
    CHECK(MarkHarness::mark_place(m.shown("preset_abs"), "preset") ==
          (state == 2 ? "name" : "none"));
    for (const char* name : kUnmarked) {
        CHECK(MarkHarness::mark_place(m.shown(name), "preset") == "none");
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "The spool preset carries the macro mark only while its material names a macro",
                 "[filament_preset_mark][2183]") {
    const bool with_macro = GENERATE(true, false);
    CAPTURE(with_macro);
    MaterialSettingsScope settings;
    MarkHarness m(*this);
    // Slot 2 gives ABS up, so a loaded ABS is a material with no button of its own.
    MaterialSettingsManager::instance().set_preset_material(2, "ASA");
    helix::SlotInfo spool;
    spool.material = "ABS";
    helix::AmsStateTestAccess::seed_external_spool(spool);
    MaterialSettingsManager::instance().set_override(
        "ABS", abs_override(with_macro ? std::optional<std::string>{"HEAT_ABS"} : std::nullopt));
    helix::ui::FilamentPanelTestAccess::update_spool_preset(m.panel);
    m.settle();

    lv_obj_t* button = m.shown("preset_spool");
    REQUIRE_FALSE(lv_obj_is_hidden(button));
    CHECK(std::string(lv_subject_get_string(
              lv_xml_get_subject(nullptr, "filament_spool_preset_name"))) == "ABS");
    CHECK(MarkHarness::mark_place(button, "spool_preset") == (with_macro ? "name" : "none"));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "The macro mark keeps each name over its temperature line and fits beside it",
                 "[filament_preset_mark][2183]") {
    MaterialSettingsScope settings;
    MarkHarness m(*this);
    MaterialSettingsManager::instance().set_override("ABS", abs_override("HEAT_ABS"));
    helix::presets::refresh_subjects();
    m.settle();

    // Marked: the glyph and the name together span one run over the temperature
    // line, the glyph before the name, both inside the button.
    lv_obj_t* abs = m.shown("preset_abs");
    lv_obj_t* mark = m.under(abs, "preset_macro_mark");
    REQUIRE(MarkHarness::mark_place(abs, "preset") == "name");
    const lv_area_t button = m.area(abs);
    const lv_area_t glyph = m.area(mark);
    const lv_area_t name = m.area(m.under(abs, "preset_material_name"));
    const lv_area_t temps = m.area(m.under(abs, "preset_temps"));
    INFO("button " << button.x1 << "," << button.y1 << "-" << button.x2 << "," << button.y2
                   << " glyph " << glyph.x1 << "-" << glyph.x2 << " name " << name.x1 << "-"
                   << name.x2 << " '" << lv_label_get_text(m.under(abs, "preset_material_name"))
                   << "' temps " << temps.x1 << "-" << temps.x2);
    CHECK(std::abs((glyph.x1 + name.x2) / 2 - MarkHarness::center_x(temps)) <= 1);
    CHECK(glyph.x1 >= button.x1);
    CHECK(name.x2 <= button.x2);
    CHECK(glyph.x2 < name.x1);

    // Unmarked: each name still sits over its own temperature line.
    for (const char* button_name : kUnmarked) {
        lv_obj_t* preset = m.shown(button_name);
        CHECK(MarkHarness::mark_place(preset, "preset") == "none");
        const lv_area_t label = m.area(m.under(preset, "preset_material_name"));
        const lv_area_t line = m.area(m.under(preset, "preset_temps"));
        CHECK(std::abs(MarkHarness::center_x(label) - MarkHarness::center_x(line)) <= 1);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "A tap and a hold on a marked preset's name reach the button",
                 "[filament_preset_mark][2183]") {
    MaterialSettingsScope settings;
    MarkHarness m(*this);
    MarkHarness::discover({"extruder", "heater_bed", "gcode_macro HEAT_ABS"});
    MaterialSettingsManager::instance().set_override("ABS", abs_override("HEAT_ABS"));
    helix::presets::refresh_subjects();
    m.settle();
    m.client.clear_gcode_script_history();

    lv_obj_t* abs = m.shown("preset_abs");
    REQUIRE(MarkHarness::mark_place(abs, "preset") == "name");
    const lv_area_t name = m.area(m.under(abs, "preset_material_name"));
    const int x = MarkHarness::center_x(name);
    const int y = (name.y1 + name.y2) / 2;
    lv_point_t at{x, y};
    lv_obj_t* hit = lv_indev_search_obj(lv_screen_active(), &at);
    INFO("pointer at " << x << "," << y << " hits " << (hit ? lv_obj_get_name(hit) : "nothing")
                       << " (button " << lv_obj_get_name(abs) << "), button disabled "
                       << lv_obj_has_state(abs, LV_STATE_DISABLED) << ", hidden "
                       << lv_obj_is_hidden(abs));
    REQUIRE(hit == abs);

    m.pointer.press(x, y);
    m.pointer.release(x, y);
    m.settle();
    CHECK(m.client.gcode_script_history() == Scripts{"HEAT_ABS"});

    m.client.clear_gcode_script_history();
    lv_obj_t* dialog = m.hold(abs);
    REQUIRE(dialog != nullptr);
    // The hold opened the picker and ended its press: the lift ran no tap.
    CHECK(m.client.gcode_script_history().empty());

    // The picker's row names what the preset runs, and opens that material's setting.
    lv_obj_t* row = lv_obj_find_by_name(dialog, "preheat_macro_row");
    REQUIRE(row != nullptr);
    REQUIRE_FALSE(lv_obj_is_hidden(row));
    CHECK_FALSE(lv_obj_is_hidden(lv_obj_find_by_name(dialog, "preheat_macro_label_heats")));
    CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(dialog, "preheat_macro_value"))) ==
          "HEAT_ABS");
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    m.settle();
    CHECK(ModalStack::instance().top_dialog() == nullptr);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "material_editing")) == 1);
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "material_edit_name"))) ==
          "ABS");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "The hold's row reads Macro for a material that names no macro yet",
                 "[filament_preset_mark][2183]") {
    MaterialSettingsScope settings;
    MarkHarness m(*this);
    m.settle();

    lv_obj_t* dialog = m.hold(m.shown("preset_abs"));
    REQUIRE(dialog != nullptr);
    // The field opens with the switch on, so the row already reads Macro, and None.
    CHECK_FALSE(lv_obj_is_hidden(m.under(dialog, "preheat_macro_label_heats")));
    CHECK(lv_obj_is_hidden(m.under(dialog, "preheat_macro_label")));
    CHECK_FALSE(lv_obj_is_hidden(m.under(dialog, "preheat_macro_none")));
    lv_obj_t* value = m.under(dialog, "preheat_macro_value");
    CHECK(lv_obj_is_hidden(value));
    CHECK(std::string(lv_label_get_text(value)) == "");

    lv_obj_send_event(lv_obj_find_by_name(dialog, "preheat_macro_row"), LV_EVENT_CLICKED, nullptr);
    m.settle();
    CHECK(ModalStack::instance().top_dialog() == nullptr);
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "material_edit_name"))) ==
          "ABS");
}

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "A preset too narrow for the mark beside its name puts the mark before its temperatures",
    "[filament_preset_mark][2183]") {
    const int height = GENERATE(320, 272);
    CAPTURE(height);
    MaterialSettingsScope settings;
    // Declared before the panel, so the panel is laid out at this size.
    helix::test::ScopedGeometry screen(480, height);
    MarkHarness m(*this);
    MaterialSettingsManager::instance().set_override("ABS", abs_override("HEAT_ABS"));
    helix::presets::refresh_subjects();
    m.settle();

    lv_obj_t* abs = m.shown("preset_abs");
    lv_obj_t* name = m.under(abs, "preset_material_name");
    lv_obj_t* glyph = m.under(abs, "preset_macro_mark_temps");
    const lv_area_t button = m.area(abs);
    const lv_area_t label = m.area(name);
    const lv_area_t mark = m.area(glyph);
    INFO("button " << button.x1 << "-" << button.x2 << " name " << label.x1 << "-" << label.x2
                   << " glyph " << mark.x1 << "-" << mark.x2);
    CHECK(MarkHarness::mark_place(abs, "preset") == "temps");
    // The name is drawn whole, inside the button, and the glyph stays inside it too.
    const int32_t whole =
        helix::ui::text_width("ABS/ASA", lv_obj_get_style_text_font(name, LV_PART_MAIN),
                              lv_obj_get_style_text_letter_space(name, LV_PART_MAIN));
    INFO("name box " << label.x2 - label.x1 + 1 << " px, the whole name draws " << whole << " px");
    CHECK(label.x2 - label.x1 + 1 >= whole - 1);
    CHECK(label.x1 > button.x1);
    CHECK(label.x2 < button.x2);
    CHECK(mark.x1 >= button.x1);
    // The temperature line shown ends inside the button, clear of the glyph.
    for (const char* line : {"preset_temps", "preset_nozzle_temp"}) {
        lv_obj_t* temps = m.under(abs, line);
        if (lv_obj_is_hidden(temps)) {
            continue;
        }
        const lv_area_t shown = m.area(temps);
        INFO(shown.x1 << "-" << shown.x2);
        CHECK(shown.x1 > mark.x2);
        CHECK(shown.x2 < button.x2);
    }
    for (const char* button_name : kUnmarked) {
        CHECK(MarkHarness::mark_place(m.shown(button_name), "preset") == "none");
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A marked preset gives its mark room on its temperature line and only then",
                 "[filament_preset_mark][2183]") {
    const bool with_macro = GENERATE(true, false);
    CAPTURE(with_macro);
    MaterialSettingsScope settings;
    // Declared before the panel, so the panel is laid out at this size.
    helix::test::ScopedGeometry screen(480, 400);
    MarkHarness m(*this);
    // ABS/ASA is too long for the mark beside it here, while its temperatures
    // are short enough for the nozzle-and-bed line to fit the button alone.
    filament::MaterialOverride override =
        abs_override(with_macro ? std::optional<std::string>{"HEAT_ABS"} : std::nullopt);
    override.nozzle_min = 200;
    override.nozzle_max = 220;
    override.bed_temp = 50;
    MaterialSettingsManager::instance().set_override("ABS", override);
    helix::presets::refresh_subjects();
    m.settle();

    lv_obj_t* abs = m.shown("preset_abs");
    lv_obj_t* full = m.under(abs, "preset_temps");
    lv_obj_t* nozzle = m.under(abs, "preset_nozzle_temp");
    const lv_area_t button = m.area(abs);
    INFO("button " << button.x1 << "-" << button.x2 << " full line " << m.area(full).x1 << "-"
                   << m.area(full).x2 << " '" << lv_label_get_text(full) << "'");
    if (with_macro) {
        CHECK(MarkHarness::mark_place(abs, "preset") == "temps");
        // The full line does not fit beside the mark, so the nozzle alone shows.
        CHECK(lv_obj_is_hidden(full));
        const lv_area_t shown = m.area(nozzle);
        CHECK(shown.x1 > m.area(m.under(abs, "preset_macro_mark_temps")).x2);
        CHECK(shown.x2 < button.x2);
    } else {
        CHECK(MarkHarness::mark_place(abs, "preset") == "none");
        // With no mark on it, the button still shows the nozzle-and-bed line.
        CHECK_FALSE(lv_obj_is_hidden(full));
        CHECK(lv_obj_is_hidden(nozzle));
    }
}
