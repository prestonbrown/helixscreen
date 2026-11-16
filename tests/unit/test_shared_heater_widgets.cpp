// SPDX-License-Identifier: GPL-3.0-or-later

// Home widgets on nozzles that share one heater (Bondtech INDX): only the
// mounted nozzle reads the heater, and the one nozzle row is named after it.

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/nozzle_temps_test_access.h"
#include "../test_helpers/tool_switcher_test_access.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/nozzle_temps_widget.h"
#include "src/ui/panel_widgets/tool_switcher_widget.h"
#include "tool_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using helix::ui::UpdateQueue;

namespace {

struct SharedHeaterWidgetFixture : LVGLUITestFixture {
    ~SharedHeaterWidgetFixture() override {
        ToolState::instance().deinit_subjects();
        UpdateQueue::instance().drain_until_empty();
    }

    /// Four nozzles on the one `extruder`, @p active mounted (-1 none).
    void seed(int active) {
        state().temperature_state().init_extruders({"extruder"});
        ToolState& ts = ToolState::instance();
        ts.deinit_subjects();
        ts.init_subjects(false);
        PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json::array({"extruder", "heater_bed", "gcode_move"}));
        ts.init_tools(hw);
        mount(active);
    }

    void mount(int active) {
        ToolTopology topo;
        topo.tool_count = 4;
        topo.active_tool = active;
        topo.allows_empty_carriage = true;
        topo.tool_to_slot = {0, 1, 2, 3};
        topo.tool_extruder = {"extruder", "extruder", "extruder", "extruder"};
        topo.heads_share_heater = true;
        ToolState::instance().set_ams_topology(topo);
        UpdateQueue::instance().drain_until_empty();
    }

    lv_obj_t* make_tile(const char* component) {
        auto* tile = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), component, nullptr));
        REQUIRE(tile != nullptr);
        return tile;
    }
};

} // namespace

TEST_CASE_METHOD(SharedHeaterWidgetFixture,
                 "Tool switcher: a docked nozzle shows no heater reading",
                 "[tool_switcher][indx][shared_heater]") {
    seed(2);
    ToolSwitcherWidget widget(state());
    widget.attach(make_tile("panel_widget_tool_switcher"), test_screen());
    state().update_from_status(nlohmann::json{{"extruder", {{"temperature", 215.0}}}});
    UpdateQueue::instance().drain_until_empty();

    CHECK_FALSE(ToolSwitcherTestAccess::head_temp_text(widget, 2).empty());
    CHECK(ToolSwitcherTestAccess::head_hot(widget, 2));
    for (int docked : {0, 1, 3}) {
        CAPTURE(docked);
        CHECK(ToolSwitcherTestAccess::head_temp_text(widget, docked).empty());
        CHECK_FALSE(ToolSwitcherTestAccess::head_hot(widget, docked));
    }
    widget.detach();
}

TEST_CASE_METHOD(SharedHeaterWidgetFixture, "Nozzle temps: the one row follows the mounted nozzle",
                 "[nozzle_temps][indx][shared_heater]") {
    seed(2);
    NozzleTempsWidget widget(state());
    widget.attach(make_tile("panel_widget_nozzle_temps"), test_screen());
    UpdateQueue::instance().drain_until_empty();
    REQUIRE(NozzleTempsTestAccess::row_count(widget) == 1);
    CHECK(NozzleTempsTestAccess::row_long_name(widget, 0) == "Nozzle 3");
    CHECK(NozzleTempsTestAccess::row_short_name(widget, 0) == "Tool 3");

    // A change of nozzle renames the row without the tool list changing.
    mount(0);
    CHECK(NozzleTempsTestAccess::row_long_name(widget, 0) == "Nozzle 1");

    // An empty head is the heater alone.
    mount(-1);
    CHECK(NozzleTempsTestAccess::row_long_name(widget, 0) == "Nozzle");
    CHECK(NozzleTempsTestAccess::row_short_name(widget, 0) == "Nozzle");
    widget.detach();
}
