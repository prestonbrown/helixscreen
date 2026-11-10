// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/printer_image_regions_test_access.h"
#include "../test_helpers/process_async_timers.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl_private.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_image_regions.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/printer_image_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

void settle() {
    for (int i = 0; i < 4; ++i) {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        process_async_timers();
    }
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a climbing bed's glow breathes in image opacity and rests at full",
                 "[printer_image][callouts]") {
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();
    ImageRegions r;
    r.src_w = 1601;
    r.src_h = 1204;
    r.nozzle = {0.513f, 0.279f};
    r.bed_left = {0.308f, 0.571f};
    r.bed_right = {0.611f, 0.573f};
    ScopedImageRegions regions({{"generic-corexy", r}});

    auto& display = DisplaySettingsManager::instance();
    const bool prev_animations = display.get_animations_enabled();
    display.set_animations_enabled(true);

    PrinterImageWidget widget;
    auto* comp =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "panel_widget_printer_image", nullptr));
    REQUIRE(comp);
    widget.attach(comp, test_screen());
    lv_obj_update_layout(comp);
    widget.on_size_changed(4, 4, 160, 160);

    auto& temps = get_printer_state().temperature_state();
    // Warm enough to glow, still climbing.
    lv_subject_set_int(temps.get_bed_target_subject(), 800);
    lv_subject_set_int(temps.get_bed_temp_subject(), 600);
    settle();

    lv_obj_t* glow = lv_obj_find_by_name(comp, "callout_bed_glow");
    REQUIRE(glow);
    lv_anim_t* pulse = lv_anim_get(glow, nullptr);
    REQUIRE(pulse != nullptr);
    CHECK(pulse->start_value == 150);
    CHECK(pulse->end_value == LV_OPA_COVER);

    pulse->exec_cb(glow, 180);
    CHECK(lv_obj_get_style_image_opa(glow, LV_PART_MAIN) == 180);

    lv_subject_set_int(temps.get_bed_target_subject(), 0);
    settle();
    CHECK(lv_anim_get(glow, nullptr) == nullptr);
    CHECK(lv_obj_get_style_image_opa(glow, LV_PART_MAIN) == LV_OPA_COVER);

    widget.detach();
    lv_obj_delete(comp);
    display.set_animations_enabled(prev_animations);
}
