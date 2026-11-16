// SPDX-License-Identifier: GPL-3.0-or-later

// The temperature overlay's nozzle presets and Custom button dim while the
// filament system refuses nozzle heat, in both orientations.

#include "ui_nav_manager.h"
#include "ui_overlay_temp_graph.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_state.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

class TempGraphHeatFixture : public LVGLUITestFixture {
  protected:
    TempGraphHeatFixture() {
        for (auto& p : panels_)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels_.data());
        helix::AmsState::instance().init_subjects(true);
    }
    ~TempGraphHeatFixture() override {
        lv_subject_set_int(helix::AmsState::instance().get_nozzle_heat_blocked_subject(), 0);
        StaticPanelRegistry::instance().destroy_all();
        settle();
    }
    void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(50);
        }
    }
    std::array<lv_obj_t*, kUiPanelCount> panels_{};
};

void collect_named(lv_obj_t* root, const std::vector<std::string>& names,
                   std::vector<lv_obj_t*>& out) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t* child = lv_obj_get_child(root, static_cast<int32_t>(i));
        const char* name = lv_obj_get_name(child);
        if (name && std::find(names.begin(), names.end(), name) != names.end()) {
            out.push_back(child);
        }
        collect_named(child, names, out);
    }
}

} // namespace

TEST_CASE_METHOD(TempGraphHeatFixture, "Temp overlay nozzle presets dim while heat is blocked",
                 "[temp_graph][indx][heat]") {
    const int portrait_mode = GENERATE(0, 1);
    CAPTURE(portrait_mode);
    lv_subject_t* portrait = lv_xml_get_subject(nullptr, "ui_is_portrait");
    REQUIRE(portrait != nullptr);
    lv_subject_set_int(portrait, portrait_mode);

    auto& overlay = get_global_temp_graph_overlay();
    overlay.open(TempGraphOverlay::Mode::Nozzle, lv_screen_active());
    settle();
    lv_obj_t* root = overlay.get_root();
    REQUIRE(root != nullptr);

    const std::vector<std::string> names = {
        "preset_1",      "preset_2",      "preset_3",          "preset_1_tiny",
        "preset_2_tiny", "preset_3_tiny", "nozzle_btn_custom", "nozzle_btn_custom_tiny"};
    std::vector<lv_obj_t*> controls;
    collect_named(root, names, controls);
    REQUIRE(controls.size() >= 4);

    lv_subject_set_int(helix::AmsState::instance().get_nozzle_heat_blocked_subject(), 1);
    settle();
    for (lv_obj_t* obj : controls) {
        CAPTURE(lv_obj_get_name(obj));
        CHECK(lv_obj_get_style_opa(obj, LV_PART_MAIN) < LV_OPA_COVER);
    }
    // "Off" turns the heater off and is never dimmed.
    if (lv_obj_t* off = lv_obj_find_by_name(root, "preset_off")) {
        CHECK(lv_obj_get_style_opa(off, LV_PART_MAIN) == LV_OPA_COVER);
    }

    NavigationManager::instance().go_back();
    settle();
    lv_subject_set_int(portrait, 0);
}
