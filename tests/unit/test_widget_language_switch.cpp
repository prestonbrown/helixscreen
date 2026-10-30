// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A home widget that sets a label from C++ with lv_tr() has translated it once,
// at that moment. XML re-translates only what it bound through translation_tag,
// so after a language switch the C++-set text stays in the old language next to
// XML text in the new one. This sweep builds every registered home widget, and
// requires every label whose English text is a translation key to read in the
// new language once the switch has settled.

#include "ui_ams_tool_text.h"
#include "ui_breakpoint.h"
#include "ui_carousel.h"
#include "ui_language_refresh.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "display_numbering.h"
#include "grid_layout.h"
#include "lvgl.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "preheat_widget.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/fan_stack_widget.h"
#include "src/ui/panel_widgets/fan_widget.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "src/ui/panel_widgets/temp_graph_widget.h"
#include "src/ui/panel_widgets/thermistor_widget.h"
#include "system_settings_manager.h"
#include "temp_graph_controller.h"
#include "temp_graph_internal.h"
#include "tool_state.h"

#include <algorithm>
#include <functional>
#include <map>
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
    state.temperature_state().init_extruders({"extruder", "extruder1"});
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

    // A backend an earlier case synced and then dropped leaves its clog meter
    // text in AmsState, and with no backend left the refresh has nothing to
    // resync it from, so the sweep starts from fresh AMS subjects.
    AmsState::instance().deinit_subjects();
    AmsState::instance().init_subjects(true);

    PanelWidgetManager::instance().init_widget_subjects();
    seed_two_hotends(state());
    // The app re-renders printer-layer text through this on every switch.
    helix::ui::init_language_refresh();

    // 800x480, measured the way test_widget_content_fits.cpp's table was.
    const GridDimensions dims = GridLayout::get_dimensions(UiBreakpoint::Medium, 710, 466);
    const CellMetrics m = grid_cell_metrics(710, 466, dims.cols, dims.rows, 5);

    auto& settings = SystemSettingsManager::instance();
    std::vector<std::string> stale;
    int checked = 0;

    for (const auto& def : get_all_widget_defs()) {
        // Smallest and one column wider: several widgets pick their wording
        // from the width they are given.
        const int min_c = def.effective_min_colspan();
        const int min_r = def.effective_min_rowspan();
        for (const int c :
             {min_c, std::min(min_c + GridLayout::TRACKS_PER_CELL, def.effective_max_colspan())}) {
            settings.set_language("en");
            helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

            RegistryWidgetHarness h(test_screen(), def, &m);
            if (!h.created()) {
                continue;
            }
            h.resize(c, min_r, static_cast<int>(grid_track_extent(m.cell_w, m.gutter, c)),
                     static_cast<int>(grid_track_extent(m.cell_h, m.gutter, min_r)));
            helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

            std::map<std::string, std::string> before;
            collect_labels(h.root(), "", before);

            settings.set_language("ru");
            helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
            // With no pack loaded every lv_tr() is the identity and the sweep
            // passes against nothing.
            REQUIRE(std::string(lv_tr("Part")) != "Part");

            std::map<std::string, std::string> after;
            collect_labels(h.root(), "", after);

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

    const auto label_en = PreheatWidget::label_for_slot(0, false, 400);
    REQUIRE(label_en.rfind("Preheat ", 0) == 0);
    REQUIRE(PreheatWidget::label_for_slot(0, true, 400) == "Cool Down");

    SystemSettingsManager::instance().set_language("ru");
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(std::string(lv_tr("All")) != "All");
    CHECK(std::string(lv_label_get_text(label)) == std::string(lv_tr("All")) + " (2)");
    REQUIRE(std::string(lv_tr("Preheat %s (%d/%d)")) != "Preheat %s (%d/%d)");
    CHECK(PreheatWidget::label_for_slot(0, false, 400)
              .rfind(std::string(lv_tr("Preheat")) + " ", 0) == 0);
    CHECK(PreheatWidget::label_for_slot(0, true, 400) == lv_tr("Cool Down"));
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

// Only a real switch runs the handler: registering, or re-selecting the same
// language, must not re-render everything that watches it.
TEST_CASE_METHOD(LanguageSwitchFixture, "observe_language_change fires on a real switch only",
                 "[i18n][observer]") {
    struct Owner {
        int renders = 0;
    } owner;
    ObserverGuard guard =
        helix::ui::observe_language_change(&owner, [](Owner* o) { ++o->renders; });
    auto drain = []() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    };
    drain();
    CHECK(owner.renders == 0);

    auto& settings = SystemSettingsManager::instance();
    settings.set_language("ru");
    drain();
    CHECK(owner.renders == 1);
    settings.set_language("ru");
    drain();
    CHECK(owner.renders == 1);
    settings.set_language("en");
    drain();
    CHECK(owner.renders == 2);
}

// The tag is what keeps a C++-written placeholder translatable: set as plain
// text it would stay in the language it was written in.
TEST_CASE_METHOD(LanguageSwitchFixture,
                 "fan and sensor tiles re-translate their unconfigured placeholder",
                 "[panel_widget][i18n]") {
    ScopedRuntimeConfig runtime_config;
    get_runtime_config()->test_mode = true;
    PanelWidgetManager::instance().init_widget_subjects();

    PanelWidgetHarness<FanWidget> fan(test_screen(), "fan");
    PanelWidgetHarness<ThermistorWidget> sensor(test_screen(), "thermistor");
    // Select something, then clear it: clearing is what writes the placeholder.
    fan.widget().select_fan("fan");
    fan.widget().select_fan("");
    sensor.widget().select_sensor("chamber");
    sensor.widget().select_sensor("");
    lv_obj_t* fan_name = fan.child("fan_name");
    lv_obj_t* sensor_name = sensor.child("thermistor_name");
    REQUIRE(fan_name != nullptr);
    REQUIRE(sensor_name != nullptr);
    REQUIRE(std::string(lv_label_get_text(fan_name)) == "Select fan");
    REQUIRE(std::string(lv_label_get_text(sensor_name)) == "Select sensor");

    SystemSettingsManager::instance().set_language("ru");
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(std::string(lv_tr("Select fan")) != "Select fan");
    CHECK(std::string(lv_label_get_text(fan_name)) == lv_tr("Select fan"));
    CHECK(std::string(lv_label_get_text(sensor_name)) == lv_tr("Select sensor"));
}

// Tool labels and extruder names are translated where
// the printer layer discovers the hardware, not by any widget.
TEST_CASE_METHOD(LanguageSwitchFixture,
                 "discovered tool and extruder names re-translate on a language switch",
                 "[i18n][tool_state]") {
    seed_two_hotends(state());
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    helix::ui::init_ams_tool_text_observers();
    helix::ui::init_language_refresh();
    lv_subject_set_int(ams.get_current_tool_subject(), 1);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    auto& tools = ToolState::instance();
    const std::string tool_en = tools.tools()[1].display_label;
    const std::string nozzle_en =
        state().temperature_state().extruders().at("extruder1").display_name;
    REQUIRE(tool_en == helix::ui::lane_label(helix::ui::active_tool_noun(), 1));
    REQUIRE(nozzle_en == "Nozzle 2");
    const int version = lv_subject_get_int(tools.get_tools_version_subject());

    SystemSettingsManager::instance().set_language("ru");
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(std::string(lv_tr("Nozzle")) != "Nozzle");

    const std::string tool_ru = helix::ui::lane_label(helix::ui::active_tool_noun(), 1);
    REQUIRE(tool_ru != tool_en);
    CHECK(tools.tools()[1].display_label == tool_ru);
    CHECK(lv_subject_get_int(tools.get_tools_version_subject()) > version);
    CHECK(state().temperature_state().extruders().at("extruder1").display_name ==
          std::string(lv_tr("Nozzle")) + " 2");
}

namespace helix {
class TempGraphWidgetTestAccess {
  public:
    static TempGraphController* controller(TempGraphWidget& w) {
        return w.controller_.get();
    }
};
} // namespace helix

// The legend names are drawn from the series metadata, so a switch has to
// rename the series, not just the labels around the graph.
TEST_CASE_METHOD(LanguageSwitchFixture, "the temp graph legend re-translates its series names",
                 "[panel_widget][i18n][temp_graph]") {
    ScopedRuntimeConfig runtime_config;
    get_runtime_config()->test_mode = true;
    PanelWidgetManager::instance().init_widget_subjects();
    seed_two_hotends(state());

    PanelWidgetHarness<TempGraphWidget> h(test_screen(), "temp_graph");
    TempGraphController* controller = TempGraphWidgetTestAccess::controller(h.widget());
    REQUIRE(controller != nullptr);
    auto name_of = [controller](const char* klipper) {
        const auto* meta = helix::temp_graph_internal::find_meta_by_id(
            controller->graph(), controller->series_id_for(klipper));
        return meta ? std::string(meta->name) : std::string("(no series)");
    };
    REQUIRE(name_of("heater_bed") == "Bed");

    SystemSettingsManager::instance().set_language("ru");
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(std::string(lv_tr("Bed")) != "Bed");
    CHECK(name_of("heater_bed") == lv_tr("Bed"));
}

// AmsState formats its status texts as it syncs from the backend, so the only
// way they follow a switch is a resync.
TEST_CASE_METHOD(LanguageSwitchFixture, "AMS status texts re-translate on a language switch",
                 "[i18n][ams]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    auto mock = std::make_unique<AmsBackendMock>();
    auto* backend = mock.get();
    backend->set_operation_delay(0);
    ams.set_backend(std::move(mock));
    backend->start();
    EncoderClogInfo encoder;
    encoder.enabled = true;
    encoder.detection_mode = 2;
    encoder.detection_length = 10.0f;
    encoder.headroom = 8.0f;
    backend->set_encoder_clog_info(encoder, 2);
    ams.sync_from_backend();
    helix::ui::init_language_refresh();
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(std::string(lv_subject_get_string(ams.get_clog_meter_mode_text_subject()))
                .rfind("Clog Auto", 0) == 0);

    SystemSettingsManager::instance().set_language("ru");
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(std::string(lv_tr("Clog Auto")) != "Clog Auto");
    CHECK(std::string(lv_subject_get_string(ams.get_clog_meter_mode_text_subject()))
              .rfind(lv_tr("Clog Auto"), 0) == 0);

    ams.set_backend(nullptr);
}
