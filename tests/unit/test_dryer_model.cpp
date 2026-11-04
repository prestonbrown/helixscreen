// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_dryer_model.cpp
 * @brief The dryer model reads the three drying sources (box dryers, the chamber cycle,
 * bed drying), publishes the `home_dryer_available` gate, and bumps its version when the
 * summary changes.
 *
 * Run with: ./build/bin/helix-tests "[dryer][model]"
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/configfile_mock_client.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/qidi_box_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_backend_qidi.h"
#include "ams_state.h"
#include "bed_drying_controller.h"
#include "moonraker_api.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "src/ui/panel_widgets/dryer_model.h"
#include "src/ui/panel_widgets/dryer_widget.h"
#include "temperature_controller.h"
#include "test_helpers/backend_harness.h"

#include <ctime>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

struct DryerModelFixture : public LVGLUITestFixture {
    DryerModelFixture() {
        reset_sources();
        dryer_model::init_subjects();
        drain();
    }
    ~DryerModelFixture() override {
        reset_sources();
        drain();
    }

    void reset_sources() {
        state().capabilities_state().set_has_chamber_dryer(false);
        state().capabilities_state().set_bed_drying(false, false);
        PanelWidgetManager::instance().unregister_shared_resource<BedDryingController>();
        // A restored bed run latches the spools on the shared printer state, and the
        // latch refuses every later test's gcode until something clears it.
        state().print_state().set_spool_latch(false, {});
    }
    void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
    static bool gate() {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, "home_dryer_available");
        REQUIRE(subject != nullptr);
        return lv_subject_get_int(subject) != 0;
    }
    static int version() {
        return lv_subject_get_int(dryer_model::version_subject());
    }
};

std::unique_ptr<helix::test::StartedBackend<AmsBackendQidi>>
boxes_one_drying(int boxes, std::time_t end_time_s) {
    auto backend = std::make_unique<helix::test::StartedBackend<AmsBackendQidi>>();
    fire_vars(*backend, json{{"box_count", boxes}});
    QidiBoxTestAccess::set_clock(*backend, [] { return std::time_t{1'000'000}; });
    QidiBoxTestAccess::set_drying_timer_supported(*backend, true);
    backend->fire_status(
        json{{"heater_generic heater_box1", json{{"temperature", 48.0}, {"target", 55.0}}},
             {"heater_generic heater_box2", json{{"temperature", 24.0}, {"target", 0.0}}}});
    QidiBoxTestAccess::apply_box_extras(
        *backend, json{{"box_drying_state", json{{"box1", json{{"end_time", end_time_s}}},
                                                 {"box2", json{{"end_time", 0}}}}}});
    return backend;
}

std::unique_ptr<helix::test::StartedBackend<AmsBackendQidi>>
two_boxes_one_drying(std::time_t end_time_s) {
    return boxes_one_drying(2, end_time_s);
}

} // namespace

// Its next init resets the subject's observer list, which would orphan the node.
TEST_CASE_METHOD(DryerModelFixture, "dryer model: a torn-down filament system is not observed",
                 "[dryer][model]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    lv_subject_t* humidity = ams.get_unit_humidity_subject(0);
    dryer_model::refresh();
    REQUIRE(lv_ll_get_len(&humidity->subs_ll) == 1);

    ams.deinit_subjects();
    dryer_model::refresh();
    CHECK(lv_ll_get_len(&humidity->subs_ll) == 0);

    ams.init_subjects(false);
    dryer_model::refresh();
    CHECK(lv_ll_get_len(&humidity->subs_ll) == 1);
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: a torn-down printer state is not observed",
                 "[dryer][model]") {
    auto& ps = state();
    lv_subject_t* active = ps.temperature_state().get_chamber_dryer_active_subject();
    dryer_model::refresh();
    REQUIRE(lv_ll_get_len(&active->subs_ll) == 1);

    ps.deinit_subjects();
    dryer_model::refresh();
    CHECK(lv_ll_get_len(&active->subs_ll) == 0);

    ps.init_subjects(true);
    dryer_model::refresh();
    CHECK(lv_ll_get_len(&active->subs_ll) == 1);
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: no source, no gate", "[dryer][model]") {
    CHECK_FALSE(gate());
    CHECK_FALSE(dryer_model::summary().available);
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: a chamber dryer opens the gate",
                 "[dryer][model]") {
    state().capabilities_state().set_has_chamber_dryer(true);
    drain();
    CHECK(gate());
    REQUIRE(dryer_model::summary().sources.size() == 1);
    CHECK(dryer_model::summary().sources[0].kind == DryerSourceKind::Chamber);
    CHECK(dryer_model::summary().drying_count == 0);

    state().capabilities_state().set_has_chamber_dryer(false);
    drain();
    CHECK_FALSE(gate());
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: bed drying opens the gate", "[dryer][model]") {
    state().capabilities_state().set_bed_drying(true, true);
    drain();
    CHECK(gate());
    REQUIRE(dryer_model::summary().sources.size() == 1);
    CHECK(dryer_model::summary().sources[0].kind == DryerSourceKind::Bed);

    // Enclosed but unable to dry on the bed: no source.
    state().capabilities_state().set_bed_drying(true, false);
    drain();
    CHECK_FALSE(gate());
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: box dryers follow the filament system",
                 "[dryer][model][ams]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ams.set_backend(two_boxes_one_drying(1'000'000 + 30 * 60));
    dryer_model::init_subjects(); // attach to the units that came up
    ams.sync_from_backend();
    drain();

    CHECK(gate());
    const DryerSummary& s = dryer_model::summary();
    REQUIRE(s.sources.size() == 2);
    CHECK(s.sources[0].kind == DryerSourceKind::Box);
    CHECK(s.sources[0].drying);
    CHECK_FALSE(s.sources[1].drying);
    CHECK(s.drying_count == 1);

    const int before = version();
    ams.sync_from_backend(); // nothing changed: nothing to announce
    drain();
    CHECK(version() == before);

    ams.deinit_subjects();
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: a running bed cycle reports its progress",
                 "[dryer][model][bed]") {
    constexpr long long kNow = 2'000'000;
    ConfigfileMockClient client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state());
    TemperatureController tc(state(), &api);

    bed_drying::RunRecord record;
    record.latched = true;
    record.material = 0;
    record.start_s = kNow - 3600;
    record.end_s = kNow + 3 * 3600;
    record.bed_c = 70;
    (void)SettingsManager::instance().set_bed_drying_record(record);

    BedDryingController ctrl(state(), &api, &tc, [] { return kNow; });
    ctrl.init_subjects();
    PanelWidgetManager::instance().register_shared_resource<BedDryingController>(&ctrl);
    state().capabilities_state().set_bed_drying(true, true);
    ctrl.restore();
    dryer_model::init_subjects(); // attach to the controller that came up
    drain();

    const DryerSummary& s = dryer_model::summary();
    REQUIRE(s.sources.size() == 1);
    CHECK(s.sources[0].kind == DryerSourceKind::Bed);
    CHECK(s.sources[0].drying);
    CHECK(s.sources[0].remaining_min == 180);
    CHECK(s.sources[0].progress_pct == 25);
    CHECK(s.sources[0].target_c == 70);

    PanelWidgetManager::instance().unregister_shared_resource<BedDryingController>();
    SettingsManager::instance().clear_bed_drying_record();
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: the version moves only when the summary does",
                 "[dryer][model]") {
    state().capabilities_state().set_has_chamber_dryer(true);
    drain();
    const int with_chamber = version();
    dryer_model::refresh();
    CHECK(version() == with_chamber);

    state().capabilities_state().set_bed_drying(true, true);
    drain();
    CHECK(version() > with_chamber);
}

TEST_CASE_METHOD(DryerModelFixture,
                 "dryer model: a chamber cycle running at the first reading draws no ring",
                 "[dryer][model][chamber]") {
    auto& temps = state().temperature_state();
    state().capabilities_state().set_has_chamber_dryer(true);
    // Before any frame the default 0 says nothing; the first real frame reports a cycle
    // that was already under way.
    lv_subject_set_int(temps.get_chamber_dryer_active_subject(), 1);
    lv_subject_set_int(temps.get_chamber_dryer_reported_subject(), 1);
    drain();
    const DryerSummary& s = dryer_model::summary();
    REQUIRE(s.sources.size() == 1);
    CHECK(s.sources[0].drying);
    CHECK(s.sources[0].progress_pct == -1);

    lv_subject_set_int(temps.get_chamber_dryer_active_subject(), 0);
    drain();
    CHECK_FALSE(dryer_model::summary().sources[0].drying);
    lv_subject_set_int(temps.get_chamber_dryer_reported_subject(), 0);
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: idle rows follow the bed and chamber temperature",
                 "[dryer][model][temps]") {
    auto& temps = state().temperature_state();
    state().capabilities_state().set_bed_drying(true, true);
    lv_subject_set_int(temps.get_bed_temp_subject(), 250);
    drain();
    REQUIRE(dryer_model::summary().sources.size() == 1);
    CHECK(dryer_model::summary().sources[0].current_c == 25);

    lv_subject_set_int(temps.get_bed_temp_subject(), 612);
    drain();
    CHECK(dryer_model::summary().sources[0].current_c == 61);

    // Less than a whole degree is not worth a refresh.
    const int before = version();
    lv_subject_set_int(temps.get_bed_temp_subject(), 614);
    drain();
    CHECK(version() == before);
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: units that come up after attach are watched",
                 "[dryer][model][ams]") {
    auto& ams = AmsState::instance();
    ams.deinit_subjects();
    dryer_model::init_subjects(); // no filament system yet
    drain();
    CHECK_FALSE(gate());

    ams.init_subjects(false);
    ams.set_backend(two_boxes_one_drying(1'000'000 + 30 * 60));
    ams.sync_from_backend();
    dryer_model::refresh(); // whatever brings the model back: a widget attaching
    drain();
    REQUIRE(dryer_model::summary().sources.size() == 2);
    CHECK(dryer_model::summary().drying_count == 1);

    // From here the per-unit subjects are watched without another init.
    ams.set_backend(two_boxes_one_drying(0));
    ams.sync_from_backend();
    drain();
    CHECK(dryer_model::summary().drying_count == 0);

    ams.deinit_subjects();
}

TEST_CASE_METHOD(DryerModelFixture,
                 "dryer model: a unit added while a tile is attached gets its row",
                 "[dryer][model][ams]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ams.set_backend(boxes_one_drying(1, 1'000'000 + 30 * 60));
    ams.sync_from_backend();
    PanelWidgetManager::instance().init_widget_subjects();
    drain();

    PanelWidgetHarness<DryerWidget> tile(test_screen());
    tile.resize(1, 1, 234, 232);
    drain();
    lv_subject_t* rows = lv_xml_get_subject(nullptr, "home_dryer_row_count");
    REQUIRE(rows != nullptr);
    CHECK(lv_subject_get_int(rows) == 1);

    ams.set_backend(boxes_one_drying(2, 1'000'000 + 30 * 60));
    ams.sync_from_backend();
    drain();
    CHECK(dryer_model::summary().sources.size() == 2);
    CHECK(lv_subject_get_int(rows) == 2);

    ams.deinit_subjects();
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: a burst of source changes queues one refresh",
                 "[dryer][model][queue]") {
    using Access = helix::ui::UpdateQueueTestAccess;
    auto& queue = helix::ui::UpdateQueue::instance();
    auto& temps = state().temperature_state();

    state().capabilities_state().set_has_chamber_dryer(true);
    const size_t one_change = Access::pending_count(queue);
    REQUIRE(one_change > 0);
    drain();

    state().capabilities_state().set_has_chamber_dryer(false);
    state().capabilities_state().set_bed_drying(true, true);
    lv_subject_set_int(temps.get_bed_temp_subject(), 300);
    CHECK(Access::pending_count(queue) == one_change);
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: attaching to the sources queues no refresh",
                 "[dryer][model][queue]") {
    state().capabilities_state().set_has_chamber_dryer(true);
    drain();

    dryer_model::refresh();

    CHECK(helix::ui::UpdateQueueTestAccess::queue_empty(helix::ui::UpdateQueue::instance()));
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: the chamber row follows whole degrees",
                 "[dryer][model][temps]") {
    auto& temps = state().temperature_state();
    state().capabilities_state().set_has_chamber_dryer(true);
    lv_subject_set_int(temps.get_chamber_temp_subject(), 250);
    drain();
    REQUIRE(dryer_model::summary().sources.size() == 1);
    CHECK(dryer_model::summary().sources[0].current_c == 25);

    lv_subject_set_int(temps.get_chamber_temp_subject(), 612);
    drain();
    CHECK(dryer_model::summary().sources[0].current_c == 61);

    // Less than a whole degree queues nothing.
    lv_subject_set_int(temps.get_chamber_temp_subject(), 614);
    CHECK(helix::ui::UpdateQueueTestAccess::queue_empty(helix::ui::UpdateQueue::instance()));
}
