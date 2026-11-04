// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_subject_initializer_shared_resources.cpp
 * @brief The shared resources a SubjectInitializer registers on PanelWidgetManager live
 *        exactly as long as that SubjectInitializer.
 *
 * A printer switch destroys one SubjectInitializer and builds the next, and the widget
 * subjects of the next printer come up before the new one registers anything. Whatever
 * PanelWidgetManager answers in that window is what they attach to.
 *
 * Run with: ./build/bin/helix-tests "[shared_resources]"
 */

#include "ui_update_queue.h"

#include "../../lvgl_ui_test_fixture.h"
#include "../../test_helpers/subject_initializer_test_access.h"
#include "bed_drying_controller.h"
#include "i_moonraker_api.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "src/ui/panel_widgets/dryer_model.h"
#include "temperature_controller.h"
#include "temperature_service.h"

#include <memory>

#include "../../catch_amalgamated.hpp"

namespace {

using helix::BedDryingController;
using helix::IMoonrakerAPI;
using helix::PanelWidgetManager;
using helix::TemperatureController;

class SharedResourceFixture : public LVGLUITestFixture {
  public:
    SharedResourceFixture() {
        reset_sources();
    }
    ~SharedResourceFixture() override {
        reset_sources();
        drain();
    }

    std::unique_ptr<SubjectInitializer> build_printer_scope() {
        auto si = std::make_unique<SubjectInitializer>();
        SubjectInitializerTestAccess::init_temperature_control(*si, api());
        return si;
    }

    void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

  private:
    void reset_sources() {
        // A persisted bed-drying run would latch the spools on restore().
        helix::SettingsManager::instance().clear_bed_drying_record();
        state().capabilities_state().set_bed_drying(false, false);
        auto& mgr = PanelWidgetManager::instance();
        mgr.unregister_shared_resource<TemperatureController>();
        mgr.unregister_shared_resource<BedDryingController>();
        mgr.unregister_shared_resource<TemperatureService>();
        mgr.unregister_shared_resource<IMoonrakerAPI>();
    }
};

} // namespace

TEST_CASE_METHOD(SharedResourceFixture,
                 "a destroyed SubjectInitializer leaves no shared resource naming what it freed",
                 "[shared_resources][printer_switch]") {
    auto& mgr = PanelWidgetManager::instance();
    auto si = build_printer_scope();
    REQUIRE(mgr.shared_resource<TemperatureController>() != nullptr);
    REQUIRE(mgr.shared_resource<BedDryingController>() != nullptr);
    REQUIRE(mgr.shared_resource<TemperatureService>() != nullptr);
    REQUIRE(mgr.shared_resource<IMoonrakerAPI>() == api());

    si.reset();

    CHECK(mgr.shared_resource<TemperatureController>() == nullptr);
    CHECK(mgr.shared_resource<BedDryingController>() == nullptr);
    CHECK(mgr.shared_resource<TemperatureService>() == nullptr);
    CHECK(mgr.shared_resource<IMoonrakerAPI>() == nullptr);
}

TEST_CASE_METHOD(SharedResourceFixture,
                 "a SubjectInitializer leaves alone a slot a newer owner has taken",
                 "[shared_resources][printer_switch]") {
    auto& mgr = PanelWidgetManager::instance();
    auto si = build_printer_scope();
    TemperatureController newer(state(), api());
    mgr.register_shared_resource<TemperatureController>(&newer);

    si.reset();

    CHECK(mgr.shared_resource<TemperatureController>() == &newer);
    mgr.unregister_shared_resource<TemperatureController>();
}

TEST_CASE_METHOD(SharedResourceFixture,
                 "the dryer model follows the next printer's bed-drying controller",
                 "[shared_resources][printer_switch][dryer][model]") {
    auto& mgr = PanelWidgetManager::instance();
    auto first = build_printer_scope();
    helix::dryer_model::init_subjects();
    drain();
    lv_subject_t* first_state = mgr.shared_resource<BedDryingController>()->get_state_subject();
    REQUIRE(lv_ll_get_len(&first_state->subs_ll) == 1);

    // The teardown frees the controller; the next printer's widget subjects come up
    // before its SubjectInitializer registers a new one.
    first.reset();
    helix::dryer_model::init_subjects();
    drain();

    auto second = build_printer_scope();
    auto* next = mgr.shared_resource<BedDryingController>();
    REQUIRE(next != nullptr);
    // Any source change after the controller comes up attaches the model to it.
    state().capabilities_state().set_bed_drying(true, true);
    drain();
    CHECK(lv_ll_get_len(&next->get_state_subject()->subs_ll) == 1);
}
