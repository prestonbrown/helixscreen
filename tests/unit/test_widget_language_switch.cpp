// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A home widget that sets a label from C++ with lv_tr() has translated it once,
// at that moment. XML re-translates only what it bound through translation_tag,
// so after a language switch the C++-set text stays in the old language next to
// XML text in the new one. This sweep builds every registered home widget, and
// requires every label whose English text is a translation key to read in the
// new language once the switch has settled.

#include "ui_breakpoint.h"
#include "ui_carousel.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/update_queue_test_access.h"
#include "grid_layout.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "preheat_widget.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/fan_stack_widget.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "system_settings_manager.h"
#include "tool_state.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Label text keyed by its child-index path from the widget root, so a label is
/// matched to itself across the switch without holding a pointer a rebuild
/// could free.
void collect_labels(lv_obj_t* obj, const std::string& path,
                    std::map<std::string, std::string>& out) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char* text = lv_label_get_text(obj);
        out[path] = text ? text : "";
    }
    const uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        collect_labels(lv_obj_get_child(obj, static_cast<int32_t>(i)),
                       path + "/" + std::to_string(i), out);
    }
}

/// Two hotends, so the widgets that only label a tool choice on a
/// multi-extruder printer have that label to show.
void seed_two_hotends(PrinterState& state) {
    ToolState::instance().deinit_subjects();
    ToolState::instance().init_subjects(false);
    PrinterDiscovery dual;
    dual.parse_objects(nlohmann::json::array({"extruder", "extruder1", "heater_bed", "fan"}));
    ToolState::instance().init_tools(dual);
    state.init_extruders({"extruder", "extruder1"});
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(ToolState::instance().has_multiple_extruders());
}

struct LanguageSwitchFixture : public LVGLUITestFixture {
    ~LanguageSwitchFixture() override {
        SystemSettingsManager::instance().set_language("en");
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        PrintStatusWidget::destroy_formatter_for_test();
        ToolState::instance().deinit_subjects();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

} // namespace

TEST_CASE_METHOD(LanguageSwitchFixture,
                 "every home widget re-translates its C++-set labels on a language switch",
                 "[panel_widget][i18n][sweep]") {
    // Test mode keeps widgets from starting real streams and workers.
    ScopedRuntimeConfig runtime_config;
    get_runtime_config()->test_mode = true;

    PanelWidgetManager::instance().init_widget_subjects();
    seed_two_hotends(state());

    // 800x480, measured the way test_widget_content_fits.cpp's table was.
    const GridDimensions dims = GridLayout::get_dimensions(UiBreakpoint::Medium);
    const CellMetrics m = grid_cell_metrics(710, 466, dims.cols, dims.rows, 5);

    auto& settings = SystemSettingsManager::instance();
    std::vector<std::string> stale;
    int checked = 0;

    for (const auto& def : get_all_widget_defs()) {
        // Smallest and one column wider: several widgets pick their wording
        // from the width they are given.
        const int min_c = def.effective_min_colspan();
        const int min_r = def.effective_min_rowspan();
        for (const int c : {min_c, std::min(min_c + 1, def.effective_max_colspan())}) {
            settings.set_language("en");
            helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

            // Built the way PanelWidgetManager does: component from the widget,
            // then attach, then the granted size.
            std::unique_ptr<PanelWidget> widget = def.factory ? def.factory(def.id) : nullptr;
            if (!widget) {
                continue;
            }
            widget->set_config(nlohmann::json::object());
            auto* root = static_cast<lv_obj_t*>(
                lv_xml_create(test_screen(), widget->get_component_name().c_str(), nullptr));
            if (!root) {
                continue;
            }
            widget->attach(root, test_screen());
            const int w_px = static_cast<int>(grid_track_extent(m.cell_w, m.gutter, c));
            const int h_px = static_cast<int>(grid_track_extent(m.cell_h, m.gutter, min_r));
            lv_obj_set_size(root, w_px, h_px);
            lv_obj_update_layout(root);
            widget->notify_size_changed(c, min_r, w_px, h_px);
            lv_obj_update_layout(root);
            helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
            struct Teardown {
                PanelWidget* w;
                lv_obj_t* obj;
                ~Teardown() {
                    w->detach();
                    lv_obj_delete(obj);
                    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
                }
            } teardown{widget.get(), root};

            std::map<std::string, std::string> before;
            collect_labels(root, "", before);

            settings.set_language("ru");
            helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
            // With no pack loaded every lv_tr() is the identity and the sweep
            // passes against nothing.
            REQUIRE(std::string(lv_tr("Part")) != "Part");

            std::map<std::string, std::string> after;
            collect_labels(root, "", after);

            for (const auto& [path, en] : before) {
                if (en.empty()) {
                    continue;
                }
                const std::string want = lv_tr(en.c_str());
                if (want == en) {
                    continue; // not a key, or translated to itself
                }
                ++checked;
                // Containing the translation is enough: a label may carry it
                // inside a longer string ("Все (2)"). A label that is no longer
                // where it was counts as stale: a rebuild must not hide one.
                const auto it = after.find(path);
                if (it == after.end() || it->second.find(want) == std::string::npos) {
                    const std::string now =
                        it == after.end() ? "(label gone from " + path + ")" : it->second;
                    stale.push_back(std::string(def.id) + " @" + std::to_string(c) + " cols: \"" +
                                    en + "\" still reads \"" + now + "\", want \"" + want + "\"");
                }
            }
        }
    }

    std::string report;
    for (const auto& s : stale) {
        report += "\n  " + s;
    }
    INFO(checked << " translatable labels checked; stale:" << report);
    CHECK(checked > 0);
    CHECK(stale.empty());
}

// The sweep only matches labels whose whole text is a key; the preheat target
// carries the key inside a count.
TEST_CASE_METHOD(LanguageSwitchFixture, "preheat's tool target re-translates on a language switch",
                 "[panel_widget][i18n][preheat]") {
    ScopedRuntimeConfig runtime_config;
    get_runtime_config()->test_mode = true;
    PanelWidgetManager::instance().init_widget_subjects();
    seed_two_hotends(state());

    PanelWidgetHarness<PreheatWidget> h(test_screen(), state());
    lv_obj_t* label = h.child("tool_target_label");
    REQUIRE(label != nullptr);
    REQUIRE(std::string(lv_label_get_text(label)) == "All (2)");

    SystemSettingsManager::instance().set_language("ru");
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(std::string(lv_tr("All")) != "All");
    CHECK(std::string(lv_label_get_text(label)) == std::string(lv_tr("All")) + " (2)");
}

// The carousel rebuilds its pages to re-render their names, which would start
// it over from the first page under the user's finger.
TEST_CASE_METHOD(LanguageSwitchFixture,
                 "the fan carousel re-translates its pages and keeps the page shown",
                 "[panel_widget][i18n][fan_stack]") {
    ScopedRuntimeConfig runtime_config;
    get_runtime_config()->test_mode = true;
    PanelWidgetManager::instance().init_widget_subjects();

    PanelWidgetHarness<FanStackWidget> h(
        test_screen(), HarnessConfig{nlohmann::json{{"display_mode", "carousel"}}}, "fan_stack",
        state());
    h.resize(2, 2, 300, 300);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    lv_obj_t* carousel = h.child("fan_carousel");
    REQUIRE(carousel != nullptr);
    REQUIRE(ui_carousel_get_page_count(carousel) >= 2);
    ui_carousel_goto_page(carousel, 1, false);
    REQUIRE(ui_carousel_get_current_page(carousel) == 1);

    SystemSettingsManager::instance().set_language("ru");
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(std::string(lv_tr("Hotend")) != "Hotend");

    CHECK(ui_carousel_get_current_page(carousel) == 1);
    std::map<std::string, std::string> labels;
    collect_labels(carousel, "", labels);
    auto shows = [&labels](const std::string& text) {
        for (const auto& [path, t] : labels) {
            if (t == text)
                return true;
        }
        return false;
    };
    CHECK(shows(lv_tr("Part")));
    CHECK(shows(lv_tr("Hotend")));
}
