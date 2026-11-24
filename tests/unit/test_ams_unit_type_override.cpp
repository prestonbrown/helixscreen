// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_unit_type_override.cpp
 * @brief A unit's picked drawing (Unit Type): the resolver input, its menu, and its setting.
 */

#include "ui_ams_device_operations_overlay.h"
#include "ui_color_picker_view.h"
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
#include "unit_colors.h"

#include <algorithm>
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
    auto unit = [](int lanes, PathTopology topology) {
        AmsUnit u;
        u.slot_count = lanes;
        u.topology = topology;
        return u;
    };
    auto ids = [](const AmsUnit& u) {
        std::set<std::string> out;
        for (const tray::ShellChoice& choice : tray::unit_shell_choices(u))
            out.insert(choice.shell_id);
        return out;
    };
    std::set<std::string> offered;
    for (int lanes = 1; lanes <= 16; ++lanes) {
        for (PathTopology topology : {PathTopology::HUB, PathTopology::LINEAR}) {
            for (const tray::ShellChoice& choice :
                 tray::unit_shell_choices(unit(lanes, topology))) {
                const tray::UnitShell* shell = tray::unit_shell_by_id(choice.shell_id);
                REQUIRE(shell != nullptr);
                CHECK(tray::unit_shell_has_art_for(*shell, lanes));
                CHECK(std::string(choice.name).size() > 0);
                offered.insert(choice.shell_id);
            }
        }
    }
    // Every baked drawing can be picked, but for the halves of a device drawn whole: a new one
    // with no choice row fails here.
    const std::set<std::string> halves{"yammu_upper", "yammu_lower"};
    for (const auto& dir : std::filesystem::directory_iterator("assets/images/units")) {
        if (dir.is_directory()) {
            const std::string id = dir.path().filename().string();
            INFO(id);
            CHECK(offered.count(id) == (halves.count(id) ? 0u : 1u));
        }
    }

    CHECK(ids(unit(4, PathTopology::HUB)).count("qidi_box") == 1);
    CHECK(ids(unit(6, PathTopology::HUB)).count("qidi_box") == 0);
    for (int lanes = 2; lanes <= 8; ++lanes)
        CHECK(ids(unit(lanes, PathTopology::HUB)).count("box_turtle") == 1);
    // A sliding selector draws lanes that share one path, so only such a unit is offered one.
    CHECK(ids(unit(6, PathTopology::LINEAR)).count("ercf_rail") == 1);
    CHECK(ids(unit(6, PathTopology::HUB)).count("ercf_rail") == 0);
    CHECK(ids(unit(6, PathTopology::HUB)).count("tradrack_rail") == 0);
    // A pick the unit is not offered keeps detection, however it was saved.
    const AmsSystemInfo hub = happy_hare_unit("MMU", "Other", 6);
    CHECK(id_of(tray::unit_shell_for(hub, 0, false, {"ercf_rail"})).empty());
    CHECK(id_of(tray::unit_shell_for(happy_hare_unit("MMU", "Other", 4), 0, false, {"yammu_upper"}))
              .empty());
}

TEST_CASE("unit type: the menu opens on the saved pick and saves the option tapped",
          "[ams][unit-type]") {
    const AmsSystemInfo info = happy_hare_unit("MMU", "Other", 4);
    const UnitTypeMenu automatic = unit_type_menu(info.units[0], "", "Automatic");
    REQUIRE(automatic.choices.size() == tray::unit_shell_choices(info.units[0]).size());
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

TEST_CASE("unit type: only picks that take effect ask a screen to lay out again",
          "[ams][unit-type]") {
    TwoPrinterConfig config;
    AmsSystemInfo one = happy_hare_unit("MMU", "Other", 4);
    AmsSystemInfo two = one;
    two.units.push_back(one.units[0]);
    two.units[1].name = "MMU 2";
    // Units coming or going with no pick in play change nothing.
    CHECK(unit_art_effective_picks(one).empty());
    CHECK(unit_art_effective_picks(one) == unit_art_effective_picks(two));

    auto& settings = SettingsManager::instance();
    settings.set_ams_unit_drawing("MMU", "qidi_box");
    CHECK(unit_art_effective_picks(two) == std::map<std::string, std::string>{{"MMU", "qidi_box"}});
    // A pick the unit is not offered draws nothing, so it lays nothing out.
    settings.set_ams_unit_drawing("MMU 2", "ercf_rail");
    CHECK(unit_art_effective_picks(two) == std::map<std::string, std::string>{{"MMU", "qidi_box"}});
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

    // A pick made while the unit's page is on screen redraws that page.
    overview().show_unit_view(1);
    settle();
    process_lvgl(80);
    REQUIRE(unit_page_drawing() == "night_owl");
    pick(1, "emu");
    CHECK(unit_page_drawing() == "emu");

    overview().show_overview();
    settle();
    process_lvgl(80);
    CHECK(tile_drawing(1) == "emu");
    CHECK(tile_drawing(0) == first);
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

TEST_CASE_METHOD(UnitTypePanel, "unit type: a pick made while the panel is closed shows on reopen",
                 "[ams][unit-type][ui_integration]") {
    install(false);
    open();
    REQUIRE(unit_page_drawing().empty());
    NavigationManager::instance().go_back();
    settle();
    process_lvgl(50);

    // Picked from Settings while the AMS panel is not on screen.
    pick(0, "qidi_box");
    open();
    CHECK(unit_page_drawing() == "qidi_box");
}

namespace {

/// The multi-unit mock with each unit numbered apart from its position, as a CFS numbers
/// its boxes 1 and 3 when box 2 is absent.
class NumberedUnits : public AmsBackendMock {
  public:
    NumberedUnits() : AmsBackendMock(4) {
        set_multi_unit_mode(true);
    }
    AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        for (size_t i = 0; i < info.units.size(); ++i)
            info.units[i].unit_index = static_cast<int>(2 * i + 1);
        if (reversed)
            std::reverse(info.units.begin(), info.units.end());
        return info;
    }
    /// The backend now lists its units the other way round, as after a resplit.
    bool reversed = false;
};

} // namespace

TEST_CASE_METHOD(UnitTypePanel, "unit type: the rows open on the viewed unit and save its pick",
                 "[ams][unit-type][ui_integration]") {
    auto mock = std::make_unique<NumberedUnits>();
    NumberedUnits* backend = mock.get();
    REQUIRE(mock->start().success());
    auto& ams = AmsState::instance();
    ams.set_backend(std::move(mock));
    ams.init_subjects(true);
    ams.sync_from_backend();
    const AmsSystemInfo info = ams.get_backend()->get_system_info();
    REQUIRE(info.units.size() == 2);
    REQUIRE(info.units[1].unit_index == 3);

    ams.set_viewed_unit(3); // the second unit's own number
    auto& overlay = ui::get_ams_device_operations_overlay();
    overlay.init_subjects();
    overlay.show(test_screen());
    settle();

    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "ams_unit_type_unit")) == 1);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "ams_unit_type_many_units")) == 1);
    // The second unit has two bays: its menu is the two-bay one.
    const UnitTypeMenu two_bay = unit_type_menu(info.units[1], "", lv_tr("Automatic"));
    CHECK(std::string(lv_subject_get_string(
              lv_xml_get_subject(nullptr, "ams_unit_type_options"))) == two_bay.options);

    lv_obj_t* dropdown = lv_obj_find_by_name(overlay.get_root(), "unit_type_dropdown");
    REQUIRE(dropdown != nullptr);
    int emu = 0;
    for (size_t i = 0; i < two_bay.choices.size(); ++i)
        emu = std::string(two_bay.choices[i].shell_id) == "emu" ? static_cast<int>(i) + 1 : emu;
    REQUIRE(emu > 0);
    // The units reorder while the dropdown is open: the tap still means what it showed.
    backend->reversed = true;
    lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(emu));
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    settle();
    CHECK(SettingsManager::instance().get_ams_unit_drawing(ams_unit_identity(info.units[1])) ==
          "emu");
    CHECK(
        SettingsManager::instance().get_ams_unit_drawing(ams_unit_identity(info.units[0])).empty());

    NavigationManager::instance().go_back();
    settle();
}

TEST_CASE_METHOD(UnitTypePanel, "unit type: the rows hide when the filament system goes away",
                 "[ams][unit-type][ui_integration]") {
    install(false);
    auto& overlay = ui::get_ams_device_operations_overlay();
    overlay.init_subjects();
    overlay.show(test_screen());
    settle();
    lv_subject_t* available = lv_xml_get_subject(nullptr, "ams_unit_type_available");
    REQUIRE(available != nullptr);
    REQUIRE(lv_subject_get_int(available) == 1);

    AmsState::instance().set_backend(nullptr);
    overlay.refresh();
    settle();
    CHECK(lv_subject_get_int(available) == 0);

    NavigationManager::instance().go_back();
    settle();
}

TEST_CASE_METHOD(UnitTypePanel, "unit type: a refresh with the pick unchanged keeps the unit page",
                 "[ams][unit-type][ui_integration]") {
    install(true);
    open();
    overview().show_unit_view(1);
    settle();
    process_lvgl(80);
    pick(1, "emu");
    REQUIRE(unit_page_drawing() == "emu");
    lv_obj_t* const slot = AmsPanelTestAccess::detail_slot(overview(), 0);
    REQUIRE(slot != nullptr);

    // Fresh slot data that changes no drawing lays nothing out again.
    AmsState::instance().bump_slots_version();
    settle();
    process_lvgl(80);
    CHECK(AmsPanelTestAccess::detail_slot(overview(), 0) == slot);
    CHECK(unit_page_drawing() == "emu");
}

TEST_CASE("unit colors: saved per printer and per unit", "[ams][unit-colors]") {
    TwoPrinterConfig config;
    auto& settings = SettingsManager::instance();
    UnitColors colors;
    colors.main = 0x2E7D3A;
    colors.accent = 0xD4A72C;
    settings.set_ams_unit_colors("Turtle_1", colors);
    CHECK(settings.get_ams_unit_colors("Turtle_1") == colors);
    CHECK(settings.get_ams_unit_colors("Turtle_2").empty());
    CHECK(Config::get_instance()->get<json>("/printers/printer-a/ams/unit_colors") ==
          json{{"Turtle_1", {{"main", "#2E7D3A"}, {"accent", "#D4A72C"}}}});

    config.use("printer-b");
    CHECK(settings.get_ams_unit_colors("Turtle_1").empty());

    config.use("printer-a");
    settings.set_ams_unit_colors("Turtle_1", UnitColors{});
    CHECK(settings.get_ams_unit_colors("Turtle_1").empty());
    CHECK(Config::get_instance()->get<json>("/printers/printer-a/ams/unit_colors") ==
          json::object());
}

namespace {

AmsSystemInfo turtle_unit() {
    AmsSystemInfo info = happy_hare_unit("Turtle_1", "", 4);
    info.type = AmsType::AFC;
    for (SlotInfo& s : info.units[0].slots) {
        s.status = SlotStatus::AVAILABLE;
        s.color_rgb = 0x40e0d0;
    }
    return info;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "unit colors: a drawing with no region masks stays as baked",
                 "[ams][unit-colors]") {
    TwoPrinterConfig config;
    AmsSystemInfo info = turtle_unit();
    info.units[0].name = "Night_Owl Turtle_1";
    info.units[0].slot_count = 2;
    info.units[0].slots.resize(2);
    const tray::UnitShell* shell = unit_art_shell_for(info, 0);
    REQUIRE(id_of(shell) == "night_owl");
    REQUIRE(shell->color_regions == 0);

    lv_obj_t* plain = unit_art_create(test_screen(), UnitArtTier::Full);
    lv_obj_set_size(plain, 230, 195);
    unit_art_set_unit(plain, info, 0);
    const std::vector<std::string> baked = unit_art_compose_order(plain);

    UnitColors colors;
    colors.main = 0x101010;
    SettingsManager::instance().set_ams_unit_colors(ams_unit_identity(info.units[0]), colors);
    CHECK(unit_art_colors(info.units[0]) == colors);
    lv_obj_t* colored = unit_art_create(test_screen(), UnitArtTier::Full);
    lv_obj_set_size(colored, 230, 195);
    unit_art_set_unit(colored, info, 0);
    CHECK(unit_art_compose_order(colored) == baked);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "unit colors: a drawing with region masks fills them with the user's colors",
                 "[ams][unit-colors]") {
    TwoPrinterConfig config;
    const tray::UnitShell* turtle = tray::unit_shell_by_id("box_turtle");
    REQUIRE(turtle != nullptr);
    tray::UnitShell shell = *turtle;
    shell.color_regions = unit_color_mask_bit(UnitArtDepth::Back, UnitColorRegion::Main) |
                          unit_color_mask_bit(UnitArtDepth::Front, UnitColorRegion::Accent);
    UnitColors colors;
    colors.main = 0x123456;
    colors.accent = 0xC02020;

    lv_obj_t* art = unit_art_create(test_screen(), UnitArtTier::Full);
    lv_obj_set_size(art, 230, 195);
    unit_art_detail::build_layers(art, unit_art_detail::data_of(art), &shell, 4,
                                  unit_color_layers(shell.color_regions, colors));

    const std::vector<std::string> order = unit_art_compose_order(art);
    REQUIRE(order.size() > 6);
    CHECK(order[0] == "back");
    CHECK(order[1] == "back_main_tint");
    CHECK(order[2] == "back_main_shade");
    const auto front = std::find(order.begin(), order.end(), "front");
    REQUIRE(front != order.end());
    REQUIRE(front + 2 < order.end());
    CHECK(*(front + 1) == "front_accent_tint");
    CHECK(*(front + 2) == "front_accent_shade");

    lv_obj_t* tint = lv_obj_find_by_name(art, "back_main_tint");
    REQUIRE(tint != nullptr);
    CHECK(lv_color_to_u32(lv_obj_get_style_image_recolor(tint, LV_PART_MAIN)) == 0xFF123456u);
    CHECK(lv_obj_get_style_image_recolor_opa(tint, LV_PART_MAIN) == LV_OPA_COVER);
    lv_obj_t* accent = lv_obj_find_by_name(art, "front_accent_tint");
    REQUIRE(accent != nullptr);
    CHECK(lv_color_to_u32(lv_obj_get_style_image_recolor(accent, LV_PART_MAIN)) == 0xFFC02020u);
}

TEST_CASE("unit colors: the menu offers the drawing's own colors, each preset and Custom",
          "[ams][unit-colors]") {
    const auto& presets = unit_color_presets();
    REQUIRE(unit_color_option_count() == static_cast<int>(presets.size()) + 2);
    const int custom = unit_color_option_count() - 1;
    CHECK(unit_color_option(UnitColors{}) == 0);
    CHECK(unit_color_option(presets[1].colors()) == 2);
    UnitColors one;
    one.panels = 0x123456;
    CHECK(unit_color_option(one) == custom);

    CHECK(unit_colors_for_option(0) == UnitColors{});
    CHECK(unit_colors_for_option(1) == presets[0].colors());
    // Custom saves nothing by itself: only a swatch the user changes does.
    CHECK_FALSE(unit_colors_for_option(custom).has_value());
    CHECK_FALSE(unit_colors_for_option(custom + 1).has_value());

    const UnitColors shown{0x111111, 0x222222, 0x333333};
    UnitColors picked;
    picked.accent = 0xABCDEF;
    CHECK(unit_colors_shown(picked, shown) == UnitColors{0x111111, 0xABCDEF, 0x333333});
}

namespace {

/// The Box Turtle and Night Owl rig behind the device operations overlay.
class UnitColorsRows : public UnitTypePanel {
  public:
    UnitColorsRows() {
        install(true);
        // The rows open on the viewed unit: the Box Turtle.
        AmsState::instance().set_viewed_unit(
            AmsState::instance().get_backend()->get_system_info().units[0].unit_index);
        overlay().init_subjects();
        overlay().show(test_screen());
        settle();
    }
    ~UnitColorsRows() override {
        NavigationManager::instance().go_back();
        settle();
    }
    static AmsDeviceOperationsOverlay& overlay() {
        return ui::get_ams_device_operations_overlay();
    }
    static int subject_int(const char* name) {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
        REQUIRE(subject != nullptr);
        return lv_subject_get_int(subject);
    }
    lv_obj_t* named(const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(overlay().get_root(), name);
        REQUIRE(obj != nullptr);
        return obj;
    }
    void choose(const char* dropdown_name, int option) {
        lv_obj_t* dropdown = named(dropdown_name);
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(option));
        lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
        settle();
    }
    /// The recolor of @p art's layer @p name, or 0 when it has none.
    static uint32_t tint_of(lv_obj_t* art, const char* name) {
        lv_obj_t* layer = lv_obj_find_by_name(art, name);
        return layer ? lv_color_to_u32(lv_obj_get_style_image_recolor(layer, LV_PART_MAIN)) &
                           0xFFFFFFu
                     : 0;
    }
};

lv_obj_t* find_on_screen(const char* name) {
    for (lv_obj_t* root : {lv_layer_top(), lv_screen_active(), lv_layer_sys()}) {
        if (lv_obj_t* obj = lv_obj_find_by_name(root, name))
            return obj;
    }
    return nullptr;
}

} // namespace

TEST_CASE_METHOD(UnitColorsRows,
                 "unit colors: the rows show only for a unit whose drawing has masks",
                 "[ams][unit-colors][ui_integration]") {
    const AmsSystemInfo info = AmsState::instance().get_backend()->get_system_info();
    REQUIRE(unit_art_shell_for(info, 0)->color_regions != 0);
    REQUIRE(unit_art_shell_for(info, 1)->color_regions == 0);

    // The rows open on the first unit, the Box Turtle.
    REQUIRE(subject_int("ams_unit_type_unit") == 0);
    CHECK(subject_int("ams_unit_colors_available") == 1);
    CHECK_FALSE(lv_obj_is_hidden(named("unit_colors_rows")));

    choose("unit_dropdown", 1);
    CHECK(subject_int("ams_unit_colors_available") == 0);
    CHECK(lv_obj_is_hidden(named("unit_colors_rows")));

    choose("unit_dropdown", 0);
    CHECK(subject_int("ams_unit_colors_available") == 1);
}

TEST_CASE_METHOD(UnitColorsRows, "unit colors: the rows follow the drawing picked under Unit Type",
                 "[ams][unit-colors][ui_integration]") {
    const AmsSystemInfo info = AmsState::instance().get_backend()->get_system_info();
    const UnitTypeMenu menu = unit_type_menu(info.units[0], "", lv_tr("Automatic"));
    int plain = 0, turtle = 0;
    for (size_t i = 0; i < menu.choices.size(); ++i) {
        const tray::UnitShell* shell = tray::unit_shell_by_id(menu.choices[i].shell_id);
        REQUIRE(shell != nullptr);
        if (shell->color_regions == 0 && plain == 0)
            plain = static_cast<int>(i) + 1;
        if (std::string(menu.choices[i].shell_id) == "box_turtle")
            turtle = static_cast<int>(i) + 1;
    }
    REQUIRE(plain > 0);
    REQUIRE(turtle > 0);
    REQUIRE(subject_int("ams_unit_colors_available") == 1);

    choose("unit_type_dropdown", plain);
    CHECK(subject_int("ams_unit_colors_available") == 0);
    choose("unit_type_dropdown", turtle);
    CHECK(subject_int("ams_unit_colors_available") == 1);
}

TEST_CASE_METHOD(UnitColorsRows, "unit colors: a pick is saved and the unit's art redraws in it",
                 "[ams][unit-colors][ui_integration]") {
    const AmsSystemInfo info = AmsState::instance().get_backend()->get_system_info();
    const std::string turtle = ams_unit_identity(info.units[0]);
    lv_obj_t* art = unit_art_create(test_screen(), UnitArtTier::Full);
    lv_obj_set_size(art, 230, 195);
    unit_art_set_unit(art, info, 0);
    REQUIRE(tint_of(art, "front_accent_tint") == 0);
    // Nothing set: the swatches show the drawing's own colors.
    const tray::UnitShell* shell = unit_art_shell_for(info, 0);
    CHECK(subject_int("ams_unit_colors_option") == 0);
    CHECK((lv_color_to_u32(
               lv_subject_get_color(lv_xml_get_subject(nullptr, "ams_unit_color_panels"))) &
           0xFFFFFFu) == shell->color_defaults[2]);

    SECTION("a preset") {
        const UnitColorPreset& preset = unit_color_presets()[0];
        choose("unit_colors_dropdown", 1);
        CHECK(SettingsManager::instance().get_ams_unit_colors(turtle) == preset.colors());
        CHECK(subject_int("ams_unit_colors_option") == 1);
        unit_art_set_unit(art, info, 0);
        CHECK(tint_of(art, "front_accent_tint") == preset.accent);
        CHECK(tint_of(art, "back_main_tint") == preset.main);
        CHECK(tint_of(art, "front_panels_tint") == preset.panels);

        choose("unit_colors_dropdown", 0);
        CHECK(SettingsManager::instance().get_ams_unit_colors(turtle).empty());
        unit_art_set_unit(art, info, 0);
        CHECK(lv_obj_find_by_name(art, "front_accent_tint") == nullptr);
    }

    SECTION("Custom keeps the colors on screen and Original stays reachable") {
        choose("unit_colors_dropdown", 1);
        const int custom = unit_color_option_count() - 1;
        choose("unit_colors_dropdown", custom);
        CHECK(subject_int("ams_unit_colors_option") == custom);
        // A refresh of the rows keeps Custom, though the colors still match the preset.
        overlay().refresh();
        settle();
        CHECK(subject_int("ams_unit_colors_option") == custom);
        CHECK(SettingsManager::instance().get_ams_unit_colors(turtle) ==
              unit_color_presets()[0].colors());

        choose("unit_colors_dropdown", 0);
        CHECK(SettingsManager::instance().get_ams_unit_colors(turtle).empty());
        CHECK(subject_int("ams_unit_colors_option") == 0);
    }

    SECTION("a swatch's picker saves only a color the user changed") {
        lv_obj_send_event(named("unit_color_main_swatch"), LV_EVENT_CLICKED, nullptr);
        settle();
        lv_obj_t* done = find_on_screen("cpm_done");
        REQUIRE(done != nullptr);
        lv_obj_send_event(done, LV_EVENT_CLICKED, nullptr);
        settle();
        // Done on the color it opened with leaves the drawing as baked.
        CHECK(SettingsManager::instance().get_ams_unit_colors(turtle).empty());
        CHECK(subject_int("ams_unit_colors_option") == 0);

        lv_obj_send_event(named("unit_color_main_swatch"), LV_EVENT_CLICKED, nullptr);
        settle();
        ColorPickerView* picker = ColorPickerView::active();
        REQUIRE(picker != nullptr);
        picker->pick_cell(10);
        const uint32_t picked = picker->color();
        REQUIRE(picked != shell->color_defaults[0]);
        done = find_on_screen("cpm_done");
        REQUIRE(done != nullptr);
        lv_obj_send_event(done, LV_EVENT_CLICKED, nullptr);
        settle();
        const UnitColors saved = SettingsManager::instance().get_ams_unit_colors(turtle);
        CHECK(saved.main == picked);
        CHECK_FALSE(saved.accent.has_value());
        CHECK_FALSE(saved.panels.has_value());
        CHECK(subject_int("ams_unit_colors_option") == unit_color_option_count() - 1);
        unit_art_set_unit(art, info, 0);
        CHECK(tint_of(art, "back_main_tint") == picked);
        CHECK(lv_obj_find_by_name(art, "front_accent_tint") == nullptr);
    }
}

TEST_CASE_METHOD(UnitTypePanel, "unit colors: the unit page on screen redraws in new colors",
                 "[ams][unit-colors][ui_integration]") {
    install(true);
    open();
    overview().show_unit_view(0);
    settle();
    process_lvgl(80);
    lv_obj_t* art = lv_obj_find_by_name(lv_screen_active(), "unit_view_art");
    REQUIRE(art != nullptr);
    REQUIRE(lv_obj_find_by_name(art, "front_accent_tint") == nullptr);

    // What the Colors rows do on a pick: save, then ask every picture to refresh.
    const UnitColorPreset& preset = unit_color_presets()[0];
    SettingsManager::instance().set_ams_unit_colors(identity(0), preset.colors());
    AmsState::instance().bump_slots_version();
    settle();
    process_lvgl(80);
    art = lv_obj_find_by_name(lv_screen_active(), "unit_view_art");
    REQUIRE(art != nullptr);
    lv_obj_t* accent = lv_obj_find_by_name(art, "front_accent_tint");
    REQUIRE(accent != nullptr);
    CHECK((lv_color_to_u32(lv_obj_get_style_image_recolor(accent, LV_PART_MAIN)) & 0xFFFFFFu) ==
          preset.accent);
}
