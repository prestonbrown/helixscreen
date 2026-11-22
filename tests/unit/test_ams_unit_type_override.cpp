// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_unit_type_override.cpp
 * @brief A unit's picked drawing (Unit Type): the resolver input, its menu, and its setting.
 */

#include "ui_nav_manager.h"
#include "ui_panel_ams_overview.h"
#include "ui_unit_art.h"
#include "ui_utils.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ams_panel_test_access.h"
#include "../test_helpers/config_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "ams_unit_shell.h"
#include "ams_unit_type_menu.h"
#include "config.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "settings_manager.h"
#include "src/ui/filament/unit_art_internal.h"

#include <filesystem>
#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

AmsSystemInfo happy_hare_unit(const char* name, const char* model, int lanes) {
    AmsSystemInfo info;
    info.type = AmsType::HAPPY_HARE;
    AmsUnit unit;
    unit.name = name;
    unit.model = model;
    unit.slot_count = lanes;
    unit.topology = PathTopology::HUB;
    for (int i = 0; i < lanes; ++i)
        unit.slots.push_back(SlotInfo{});
    info.units.push_back(unit);
    return info;
}

std::string id_of(const tray::UnitShell* shell) {
    return shell ? shell->id : "";
}

/// Two printers, printer-a active; the config is put back as it was on the way out.
class TwoPrinterConfig {
  public:
    TwoPrinterConfig() : cfg_(Config::get_instance()) {
        saved_data_ = ConfigTestAccess::data(*cfg_);
        saved_active_ = ConfigTestAccess::active_printer_id(*cfg_);
        json data;
        data["config_version"] = kCurrentConfigVersion;
        data["active_printer_id"] = "printer-a";
        data["printers"]["printer-a"]["printer_name"] = "A";
        data["printers"]["printer-b"]["printer_name"] = "B";
        ConfigTestAccess::data(*cfg_) = data;
        ConfigTestAccess::active_printer_id(*cfg_) = "printer-a";
    }
    ~TwoPrinterConfig() {
        ConfigTestAccess::data(*cfg_) = saved_data_;
        ConfigTestAccess::active_printer_id(*cfg_) = saved_active_;
    }
    void use(const char* printer) {
        ConfigTestAccess::active_printer_id(*cfg_) = printer;
    }

  private:
    Config* cfg_;
    json saved_data_;
    std::string saved_active_;
};

} // namespace

TEST_CASE("unit type: a pick with art for the unit's bays is the drawing", "[ams][unit-type]") {
    // Happy Hare on a QIDI Box running a community config: nothing names the hardware.
    const AmsSystemInfo info = happy_hare_unit("MMU", "Other", 4);
    REQUIRE(id_of(tray::unit_shell_for(info, 0)).empty());

    CHECK(id_of(tray::unit_shell_for(info, 0, false, {"qidi_box"})) == "qidi_box");
    CHECK(id_of(tray::device_shell_for(info, 0, false, {"qidi_box"}).shell) == "qidi_box");
    CHECK(id_of(tray::unit_shell_for(info, 0, false, {"box_turtle"})) == "box_turtle");
}

TEST_CASE("unit type: Automatic is detection", "[ams][unit-type]") {
    for (const char* model : {"QIDI", "BoxTurtle", "EMU", "Other"}) {
        INFO(model);
        const AmsSystemInfo info = happy_hare_unit("MMU", model, 4);
        const std::string detected = id_of(tray::unit_shell_for(info, 0));
        CHECK(id_of(tray::unit_shell_for(info, 0, false, {""})) == detected);
        CHECK(id_of(tray::unit_shell_for(info, 0, false, {})) == detected);
        CHECK(id_of(tray::device_shell_for(info, 0, false, {""}).shell) == detected);
    }
}

TEST_CASE("unit type: a pick without art for the unit's bays keeps detection", "[ams][unit-type]") {
    // The QIDI Box is drawn with four bays only; a Box Turtle from two to eight.
    const AmsSystemInfo six = happy_hare_unit("MMU", "BoxTurtle", 6);
    CHECK(id_of(tray::unit_shell_for(six, 0, false, {"qidi_box"})) == "box_turtle");
    CHECK(id_of(tray::unit_shell_for(six, 0, false, {"no_such_unit"})) == "box_turtle");
}

TEST_CASE("unit type: a picked unit is drawn alone, never as a slice of a device",
          "[ams][unit-type]") {
    AmsSystemInfo info = happy_hare_unit("YAMMU", "", 4);
    info.units.push_back(info.units[0]);
    REQUIRE(tray::device_shell_for(info, 1).unit_indices.size() == 2);

    const tray::ShellChoices choices{"", "qidi_box"};
    const tray::DeviceShell picked = tray::device_shell_for(info, 1, false, choices);
    CHECK(id_of(picked.shell) == "qidi_box");
    CHECK(picked.unit_indices == std::vector<int>{1});
    // Its neighbor keeps its own detection and no longer counts the picked unit as its own.
    const tray::DeviceShell other = tray::device_shell_for(info, 0, false, choices);
    CHECK(id_of(other.shell) == "yammu");
    CHECK(other.unit_indices == std::vector<int>{0});
}

TEST_CASE("unit type: the choices are every drawing with art for the unit's bays",
          "[ams][unit-type]") {
    std::set<std::string> offered;
    for (int lanes = 1; lanes <= 16; ++lanes) {
        for (const tray::ShellChoice& choice : tray::unit_shell_choices(lanes)) {
            const tray::UnitShell* shell = tray::unit_shell_by_id(choice.shell_id);
            REQUIRE(shell != nullptr);
            CHECK(tray::unit_shell_has_art_for(*shell, lanes));
            CHECK(std::string(choice.name).size() > 0);
            offered.insert(choice.shell_id);
        }
    }
    // Every baked drawing can be picked: a new one with no choice row fails here.
    for (const auto& dir : std::filesystem::directory_iterator("assets/images/units")) {
        if (dir.is_directory()) {
            INFO(dir.path().filename().string());
            CHECK(offered.count(dir.path().filename().string()) == 1);
        }
    }

    auto ids = [](int lanes) {
        std::set<std::string> out;
        for (const tray::ShellChoice& choice : tray::unit_shell_choices(lanes))
            out.insert(choice.shell_id);
        return out;
    };
    CHECK(ids(4).count("qidi_box") == 1);
    CHECK(ids(6).count("qidi_box") == 0);
    for (int lanes = 2; lanes <= 8; ++lanes)
        CHECK(ids(lanes).count("box_turtle") == 1);
}

TEST_CASE("unit type: the menu opens on the saved pick and saves the option tapped",
          "[ams][unit-type]") {
    const AmsSystemInfo info = happy_hare_unit("MMU", "Other", 4);
    const UnitTypeMenu automatic = unit_type_menu(info.units[0], "", "Automatic");
    REQUIRE(automatic.choices.size() == tray::unit_shell_choices(4).size());
    CHECK(automatic.selected == 0);
    CHECK(automatic.options.rfind("Automatic\n", 0) == 0);
    CHECK(unit_type_menu_choice(automatic, 0).empty());

    const UnitTypeMenu qidi = unit_type_menu(info.units[0], "qidi_box", "Automatic");
    REQUIRE(qidi.selected > 0);
    CHECK(std::string(qidi.choices[static_cast<size_t>(qidi.selected) - 1].name) == "QIDI Box");
    CHECK(unit_type_menu_choice(qidi, qidi.selected) == "qidi_box");
    CHECK(unit_type_menu_choice(qidi, static_cast<int>(qidi.choices.size()) + 1).empty());
    // A saved pick the unit's bays have no art for opens on Automatic.
    CHECK(unit_type_menu(happy_hare_unit("MMU", "Other", 6).units[0], "qidi_box", "Automatic")
              .selected == 0);
}

TEST_CASE("unit type: the pick is saved per printer and per unit", "[ams][unit-type]") {
    TwoPrinterConfig config;
    auto& settings = SettingsManager::instance();
    settings.set_ams_unit_drawing("MMU", "qidi_box");
    CHECK(settings.get_ams_unit_drawing("MMU") == "qidi_box");
    CHECK(settings.get_ams_unit_drawing("Turtle_1").empty());

    config.use("printer-b");
    CHECK(settings.get_ams_unit_drawing("MMU").empty());
    settings.set_ams_unit_drawing("MMU", "emu");

    config.use("printer-a");
    CHECK(settings.get_ams_unit_drawing("MMU") == "qidi_box");
    CHECK(Config::get_instance()->get<json>("/printers/printer-a/ams/unit_drawings") ==
          json{{"MMU", "qidi_box"}});

    settings.set_ams_unit_drawing("MMU", "");
    CHECK(settings.get_ams_unit_drawing("MMU").empty());
    config.use("printer-b");
    CHECK(settings.get_ams_unit_drawing("MMU") == "emu");
}

TEST_CASE_METHOD(LVGLTestFixture, "unit type: the pick changes the drawing, not the backend",
                 "[ams][unit-type]") {
    TwoPrinterConfig config;
    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_unit_model("Other", PathTopology::HUB);
    REQUIRE(mock->start().success());
    AmsBackendMock* backend = mock.get();
    AmsState::instance().init_subjects(true);
    AmsState::instance().set_backend(std::move(mock));

    const AmsSystemInfo before = backend->get_system_info();
    REQUIRE_FALSE(unit_art_available(before, 0));

    SettingsManager::instance().set_ams_unit_drawing(ams_unit_identity(before.units[0]),
                                                     "qidi_box");
    const AmsSystemInfo after = backend->get_system_info();
    CHECK(unit_art_available(after, 0));
    CHECK(id_of(unit_art_shell_for(after, 0)) == "qidi_box");

    // What the backend reports, and so how it is driven, is what it was.
    CHECK(after.type == before.type);
    CHECK(after.units[0].model == "Other");
    CHECK(after.units[0].name == before.units[0].name);
    CHECK(after.units[0].topology == PathTopology::HUB);
    CHECK(backend->get_topology() == PathTopology::HUB);
    CHECK(backend->get_unit_topology(0) == PathTopology::HUB);
    CHECK(after.units[0].slot_count == before.units[0].slot_count);

    AmsState::instance().set_backend(nullptr);
    ui::UpdateQueue::instance().drain();
}

namespace {

/// The drawing a unit_art widget shows, "" when none.
std::string drawn_id(lv_obj_t* art) {
    if (!art || lv_obj_is_hidden(art))
        return "";
    const auto* data = unit_art_detail::data_of(art);
    return data && data->built_id ? data->built_id : "";
}

/// The real AMS screens against the mock: @p multi_unit puts a Box Turtle and a Night Owl on
/// the overview; otherwise one Happy Hare unit of vendor "Other", which nothing names, opens
/// straight on its detail page.
class UnitTypePanel : public LVGLUITestFixture {
  public:
    explicit UnitTypePanel() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);
        helix::ui::destroy_static_panels();
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = lv_obj_create(test_screen());
        panels[static_cast<int>(PanelId::Controls)] = lv_obj_create(test_screen());
        NavigationManager::instance().set_panels(panels);
        UITest::init(test_screen());
    }
    ~UnitTypePanel() override {
        UITest::cleanup();
        helix::ui::destroy_static_panels();
        ui::UpdateQueue::instance().drain();
        AmsState::instance().set_backend(nullptr);
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    void install(bool multi_unit) {
        auto mock = std::make_unique<AmsBackendMock>(4);
        if (multi_unit)
            mock->set_multi_unit_mode(true);
        else
            mock->set_unit_model("Other", PathTopology::HUB);
        REQUIRE(mock->start().success());
        AmsState::instance().set_backend(std::move(mock));
        AmsState::instance().init_subjects(true);
        AmsState::instance().sync_from_backend();
    }
    std::string identity(int unit) {
        return ams_unit_identity(
            AmsState::instance().get_backend()->get_system_info().units[static_cast<size_t>(unit)]);
    }
    void open() {
        navigate_to_ams_panel();
        settle();
        process_lvgl(80);
    }
    /// Save @p shell_id for @p unit and ask for the refresh Unit Type asks for.
    void pick(int unit, const char* shell_id) {
        SettingsManager::instance().set_ams_unit_drawing(identity(unit), shell_id);
        AmsState::instance().bump_slots_version();
        settle();
        process_lvgl(80);
    }
    void settle() {
        ui::UpdateQueue::instance().drain();
        ui::UpdateQueue::instance().drain();
    }
    AmsOverviewPanel& overview() {
        auto* p = helix::lazy_global_if_exists<AmsOverviewPanel>();
        REQUIRE(p != nullptr);
        return *p;
    }
    /// The drawing on the overview tile of the unit at @p unit.
    std::string tile_drawing(int unit) {
        const auto tiles = AmsPanelTestAccess::unit_cards(overview());
        const auto units = AmsPanelTestAccess::unit_card_units(overview());
        for (size_t i = 0; i < tiles.size(); ++i) {
            if (units[i] == unit)
                return drawn_id(lv_obj_find_by_name(tiles[i], "unit_art"));
        }
        FAIL("no tile for unit " << unit);
        return "";
    }
    /// The drawing behind the spools of the unit page on screen.
    std::string unit_page_drawing() {
        return drawn_id(lv_obj_find_by_name(lv_screen_active(), "unit_view_art"));
    }

    TwoPrinterConfig config_;

  private:
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(UnitTypePanel, "unit type: the overview and its unit page draw the pick",
                 "[ams][unit-type][ui_integration]") {
    install(true);
    open();
    REQUIRE(tile_drawing(1) == "night_owl");
    const std::string first = tile_drawing(0);
    REQUIRE_FALSE(first.empty());

    pick(1, "emu");
    CHECK(tile_drawing(1) == "emu");
    CHECK(tile_drawing(0) == first);

    overview().show_unit_view(1);
    settle();
    process_lvgl(80);
    CHECK(unit_page_drawing() == "emu");
}

TEST_CASE_METHOD(UnitTypePanel, "unit type: the detail page draws the pick",
                 "[ams][unit-type][ui_integration]") {
    install(false);
    open();
    REQUIRE(unit_page_drawing().empty());

    pick(0, "qidi_box");
    CHECK(unit_page_drawing() == "qidi_box");

    pick(0, "");
    CHECK(unit_page_drawing().empty());
}
