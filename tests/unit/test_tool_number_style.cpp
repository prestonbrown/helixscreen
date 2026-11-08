// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_number_style.cpp
 * @brief Each printer numbers its tools the way the machine is labeled (#2014).
 *
 * A klipper-toolchanger rig whose `[tool]` sections are all T<n> reads "T0",
 * "T1" on every tool-number surface; everything else stays 1-based, and a
 * per-printer setting overrides either.
 */

#include "ui_ams_tool_text.h"
#include "ui_language_refresh.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "ams_backend_ace.h"
#include "ams_backend_mock.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "ams_tool_topology.h"
#include "config.h"
#include "core/static_subject_registry.h"
#include "core/update_queue.h"
#include "display_numbering.h"
#include "indx_contract.h"
#include "printer_discovery.h"
#include "settings_manager.h"
#include "tool_state.h"
#include "toolchanger_addon.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsBackendToolChanger;
using helix::AmsState;
using helix::PrinterDiscovery;
using helix::SettingsManager;
using helix::ToolState;
using helix::ui::ToolNumberStyle;
using helix::ui::ToolNumberStyleSetting;
using json = nlohmann::json;
namespace addon = helix::toolchanger_addon;

namespace {

PrinterDiscovery discovery_of(const json& objects) {
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

/// klipper-toolchanger with four tools named the way its owner wrote them.
PrinterDiscovery changer_named(const std::vector<std::string>& tools) {
    json objects = json::array({"toolchanger", "heater_bed", "toolhead", "gcode_move"});
    for (size_t i = 0; i < tools.size(); ++i) {
        objects.push_back("tool " + tools[i]);
        objects.push_back(i == 0 ? std::string("extruder") : "extruder" + std::to_string(i));
    }
    return discovery_of(objects);
}

/// A Creator 5 Pro on Z-Mod: four heads, no `[tool]` sections.
PrinterDiscovery zmod_c5() {
    json objects = json::array({"zmod", "zmod_color", "save_variables", "extruder", "extruder1",
                                "extruder2", "extruder3", "heater_bed", "toolhead", "gcode_move"});
    for (int i = 1; i <= 4; ++i) {
        objects.push_back("gcode_button extruder_pos" + std::to_string(i));
        objects.push_back("gcode_button extruder_grab" + std::to_string(i));
    }
    return discovery_of(objects);
}

/// A tool changer fed a status frame the way Moonraker delivers one, so a resync
/// of AmsState reads the mounted tool from it.
struct FedChanger : public AmsBackendToolChanger {
    using AmsBackendToolChanger::AmsBackendToolChanger;
    void feed(const json& status) {
        handle_status_update(
            json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
    }
};

/// The real badge observers and the display-text refresh over real subjects, with
/// the active backend and ToolState built from one discovery.
struct ToolStyleFixture : public LVGLUITestFixture {
    ToolStyleFixture() {
        SettingsManager::instance().init_subjects();
        SettingsManager::instance().set_tool_number_style_setting(ToolNumberStyleSetting::Auto);
        AmsState::instance().init_subjects(true);
        ToolState::instance().deinit_subjects();
        ToolState::instance().init_subjects(false);
        helix::ui::init_ams_tool_text_observers();
        helix::ui::init_language_refresh();
    }
    ~ToolStyleFixture() override {
        SettingsManager::instance().set_tool_number_style_setting(ToolNumberStyleSetting::Auto);
        helix::ui::UpdateQueue::instance().drain_until_empty();
        StaticSubjectRegistry::instance().deinit_one("AmsToolTextObservers");
        StaticSubjectRegistry::instance().deinit_one("LanguageRefresh");
        AmsState::instance().clear_backends();
        AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    /// Discovery as the app runs it: the backend learns the hardware, then the tools.
    void discover_changer(const PrinterDiscovery& hw, int active_tool) {
        const json status = {{"toolchanger", {{"tool_number", active_tool}}}};
        auto backend = std::make_unique<FedChanger>(nullptr, nullptr);
        backend->set_discovery(hw);
        backend->feed(status);
        AmsState::instance().set_backend(std::move(backend));
        auto& ts = ToolState::instance();
        ts.init_tools(hw);
        ts.update_from_status(status);
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    static std::string badge_text() {
        return lv_subject_get_string(ToolState::instance().get_tool_badge_text_subject());
    }
    static int badge_shown() {
        return lv_subject_get_int(ToolState::instance().get_show_tool_badge_subject());
    }
    static void set_style(ToolNumberStyleSetting setting) {
        SettingsManager::instance().set_tool_number_style_setting(setting);
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

} // namespace

TEST_CASE("tools are named by G-code only when every name has the T<n> shape",
          "[toolchanger][tool_style]") {
    CHECK(addon::tools_named_by_gcode({"T0", "T1", "T2"}));
    CHECK(addon::tools_named_by_gcode({"T0", "T5"}));
    CHECK_FALSE(addon::tools_named_by_gcode({"T0", "Left"}));
    CHECK_FALSE(addon::tools_named_by_gcode({}));
    CHECK_FALSE(addon::tools_named_by_gcode({"T"}));
    CHECK_FALSE(addon::tools_named_by_gcode({"t0"}));
    CHECK_FALSE(addon::tools_named_by_gcode({"T0a"}));
}

TEST_CASE("tool names discovery invented from extruders stay 1-based",
          "[toolchanger][tool_style]") {
    const auto hw = discovery_of(json::array({"extruder", "extruder1", "heater_bed", "toolhead"}));
    // The premise: discovery invents T<n> names without any [tool] section.
    REQUIRE(addon::tools_named_by_gcode(hw.tool_names()));
    CHECK(addon::tool_number_style(hw) == ToolNumberStyle::Physical);
}

TEST_CASE("klipper-toolchanger with T<n> sections numbers by G-code", "[toolchanger][tool_style]") {
    const auto hw = changer_named({"T0", "T1", "T2", "T3"});
    CHECK(addon::tool_number_style(hw) == ToolNumberStyle::Gcode);

    AmsBackendToolChanger backend(nullptr, nullptr);
    CHECK(backend.tool_number_style() == ToolNumberStyle::Physical);
    backend.set_discovery(hw);
    CHECK(backend.traits().tool_number_style == ToolNumberStyle::Gcode);
    // Only the style differs from the constant traits.
    helix::BackendTraits rest = backend.traits();
    rest.tool_number_style = ToolNumberStyle::Physical;
    CHECK(rest == AmsBackendToolChanger::kTraits);
}

TEST_CASE("klipper-toolchanger with a configured tool name stays 1-based",
          "[toolchanger][tool_style]") {
    CHECK(addon::tool_number_style(changer_named({"T0", "Left"})) == ToolNumberStyle::Physical);
}

TEST_CASE("a Creator 5, a MedusaHC and a U1 keep their 1-based heads",
          "[toolchanger][tool_style]") {
    CHECK(addon::tool_number_style(zmod_c5()) == ToolNumberStyle::Physical);
    // MedusaHC runs klipper-toolchanger with T<n> sections, but its docks'
    // labeling is not known: the add-on that claims it keeps it 1-based.
    const auto medusa = discovery_of(json::array({"toolchanger", "tool T0", "tool T1", "medusahc",
                                                  "pin_watch io", "extruder", "extruder1"}));
    REQUIRE(medusa.has_tool_changer());
    CHECK(addon::tool_number_style(medusa) == ToolNumberStyle::Physical);
    AmsBackendToolChanger c5(nullptr, nullptr);
    c5.set_discovery(zmod_c5());
    CHECK(c5.tool_number_style() == ToolNumberStyle::Physical);

    CHECK(helix::AmsBackendSnapmaker::kTraits.tool_number_style == ToolNumberStyle::Physical);
    helix::AmsBackendMock u1(4);
    u1.set_snapmaker_mode(true);
    CHECK(u1.tool_number_style() == ToolNumberStyle::Physical);
    helix::AmsBackendMock changer(4);
    changer.set_tool_changer_mode(true);
    CHECK(changer.tool_number_style() == ToolNumberStyle::Physical);
}

TEST_CASE_METHOD(ToolStyleFixture, "the nozzle badge follows the tool number style",
                 "[tool_state][tool_style]") {
    discover_changer(changer_named({"T0", "T1", "T2", "T3"}), 1);
    REQUIRE(ToolState::instance().active_tool_index() == 1);
    REQUIRE(badge_shown() == 1);
    CHECK(helix::ui::active_tool_number_style() == ToolNumberStyle::Gcode);
    CHECK(badge_text() == "T1");

    // The setting re-renders the badge on its own, with no new discovery.
    set_style(ToolNumberStyleSetting::Physical);
    CHECK(badge_text() == "2");
    set_style(ToolNumberStyleSetting::Auto);
    CHECK(badge_text() == "T1");
}

TEST_CASE_METHOD(ToolStyleFixture, "a configured tool name is the nozzle badge in either style",
                 "[tool_state][tool_style]") {
    // No backend: the tool list stays as discovery named it.
    auto& ts = ToolState::instance();
    ts.init_tools(changer_named({"Left", "Right"}));
    ts.update_from_status({{"toolchanger", {{"tool_number", 1}}}});
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(badge_text() == "Right");
    set_style(ToolNumberStyleSetting::Gcode);
    CHECK(badge_text() == "Right");
    CHECK(ts.nozzle_label() == "Nozzle Right");
}

TEST_CASE_METHOD(ToolStyleFixture, "the nozzle label follows the tool number style",
                 "[tool_state][tool_style]") {
    discover_changer(changer_named({"T0", "T1", "T2", "T3"}), 1);
    auto& ts = ToolState::instance();
    CHECK(ts.nozzle_label() == "Nozzle T1");
    CHECK(ts.tools()[1].display_label == "T1");
    set_style(ToolNumberStyleSetting::Physical);
    CHECK(ts.nozzle_label() == "Nozzle 2");
    CHECK(ts.tools()[1].display_label == "Tool 2");
}

TEST_CASE_METHOD(ToolStyleFixture, "extruder names follow the tool number style",
                 "[tool_state][tool_style]") {
    discover_changer(changer_named({"T0", "T1"}), 0);
    auto& temps = state().temperature_state();
    temps.init_extruders({"extruder", "extruder1"});
    CHECK(temps.extruders().at("extruder1").display_name == "Nozzle T1");
    set_style(ToolNumberStyleSetting::Physical);
    CHECK(temps.extruders().at("extruder1").display_name == "Nozzle 2");
}

TEST_CASE_METHOD(ToolStyleFixture, "a hub printer is untouched by a forced G-code style",
                 "[tool_state][tool_style]") {
    AmsState::instance().set_backend(std::make_unique<helix::AmsBackendAce>(nullptr, nullptr));
    auto& ts = ToolState::instance();
    ts.init_tools(discovery_of(json::array({"extruder", "heater_bed", "toolhead"})));
    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 2;
    topo.tool_to_slot = {0, 1, 2, 3};
    ts.set_ams_topology(topo);
    set_style(ToolNumberStyleSetting::Gcode);

    // A tool behind a hub is named by its lane, which has no tool-number style.
    CHECK(helix::ui::fed_lane_tool_label(helix::ui::lane_label(helix::ui::LaneNoun::Slot, 0), 0,
                                         helix::ui::ToolLabelForm::Full) == "Slot 1 (T0)");
    CHECK(badge_shown() == 0);
    CHECK(badge_text().empty());
    CHECK(ts.nozzle_label() == "Nozzle");
}

TEST_CASE("a tool changer's lane is the tool, spelled in the tool number style", "[tool_style]") {
    using helix::ui::LaneNoun;
    using helix::ui::tool_lane_label;
    CHECK(tool_lane_label(LaneNoun::Tool, ToolNumberStyle::Gcode, 0) == "T0");
    CHECK(tool_lane_label(LaneNoun::Toolhead, ToolNumberStyle::Gcode, 2) == "T2");
    CHECK(tool_lane_label(LaneNoun::Tool, ToolNumberStyle::Physical, 0) == "Tool 1");
    // A lane that is not a tool keeps its number in either style.
    CHECK(tool_lane_label(LaneNoun::Slot, ToolNumberStyle::Gcode, 2) == "Slot 3");
    CHECK(tool_lane_label(LaneNoun::Feeder, ToolNumberStyle::Gcode, 0) == "Feeder 1");
    CHECK(tool_lane_label(LaneNoun::Tool, ToolNumberStyle::Gcode, -1).empty());
}

TEST_CASE_METHOD(ToolStyleFixture, "the AMS header names the mounted tool like the badge",
                 "[ams][tool_style]") {
    discover_changer(changer_named({"T0", "T1", "T2", "T3"}), 1);
    AmsState::instance().sync_from_backend();
    helix::ui::UpdateQueue::instance().drain_until_empty();
    const auto header = [] {
        return std::string(
            lv_subject_get_string(AmsState::instance().get_current_slot_text_subject()));
    };
    REQUIRE(badge_text() == "T1");
    CHECK(header() == "T1");
    set_style(ToolNumberStyleSetting::Physical);
    CHECK(badge_text() == "2");
    CHECK(header() == "Tool 2");
}

TEST_CASE_METHOD(ToolStyleFixture, "a tool changer's own messages follow the tool number setting",
                 "[toolchanger][tool_style]") {
    // A T<n> rig whose docks report a failed swap, read through the fault dialog.
    helix::test::ToolChangerHelper tc(4);
    tc.set_discovery(changer_named({"T0", "T1", "T2", "T3"}));
    tc.set_tool_sensor(addon::resolve_tool_sensor(
        discovery_of(json::array({"toolchanger", "tool T0", "tool T1", "tool T2", "tool T3",
                                  "pin_watch io", "servo my_servo", "extruder"}))));
    const std::string failure = "MHC_SET: sensors did not confirm T1";
    tc.feed(json{{"medusahc", {{"operation", "picking"}, {"current_tool", 0}}}});
    tc.feed(json{{"medusahc",
                  {{"operation", "idle"},
                   {"current_tool", -2},
                   {"target_tool", 1},
                   {"sensor_error", true},
                   {"last_error", failure}}}});
    const auto detail = [&tc] {
        const auto err = tc.current_error();
        return err ? err->detail : std::string();
    };

    // Auto: the G-code names the docks carry.
    CHECK(tc.display_tool_number_style() == ToolNumberStyle::Gcode);
    CHECK(detail() == "Tool change to T1 failed: " + failure);

    // Forced to "1, 2, 3": the backend's text follows the setting, not its hint.
    set_style(ToolNumberStyleSetting::Physical);
    CHECK(tc.display_tool_number_style() == ToolNumberStyle::Physical);
    CHECK(detail() == "Tool change to Tool 2 (T1) failed: " + failure);
}

TEST_CASE_METHOD(ToolStyleFixture, "the setting's any-thread copy follows every set and reload",
                 "[settings][tool_style]") {
    auto& settings = SettingsManager::instance();
    CHECK(settings.get_tool_number_style_setting_any_thread() == ToolNumberStyleSetting::Auto);
    settings.set_tool_number_style_setting(ToolNumberStyleSetting::Gcode);
    CHECK(settings.get_tool_number_style_setting_any_thread() == ToolNumberStyleSetting::Gcode);
    settings.set_tool_number_style_setting(ToolNumberStyleSetting::Physical);
    CHECK(settings.get_tool_number_style_setting_any_thread() == ToolNumberStyleSetting::Physical);
    // A reload (printer switch) loads another value into the copy.
    Config* config = Config::get_instance();
    config->set<int>(config->df() + "appearance/tool_number_style", 2);
    settings.reload_from_config();
    REQUIRE(settings.get_tool_number_style_setting() == ToolNumberStyleSetting::Gcode);
    CHECK(settings.get_tool_number_style_setting_any_thread() == ToolNumberStyleSetting::Gcode);
}

TEST_CASE_METHOD(ToolStyleFixture, "a 1-based backend's own text follows a forced G-code style",
                 "[tool_style]") {
    helix::AmsBackendMock u1(4);
    u1.set_snapmaker_mode(true);
    CHECK(u1.display_tool_number_style() == ToolNumberStyle::Physical);
    CHECK(helix::indx::tool_text(0) == "Tool 1 (T0)");

    set_style(ToolNumberStyleSetting::Gcode);
    CHECK(u1.display_tool_number_style() == ToolNumberStyle::Gcode);
    CHECK(helix::indx::tool_text(0) == "T0");
}
