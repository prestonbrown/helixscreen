// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_widget_runout_dispatch.cpp
 * @brief The fourth filament dispatch surface: the IDLE runout dialog.
 *
 * Run with: ./build/bin/helix-tests "[print_status_widget][dispatch]"
 *
 * FilamentPanel, AmsOperationSidebar and FilamentRunoutHandler all route Load
 * through plan_load() (see test_filament_op_dispatch.cpp and
 * test_filament_dispatch_surfaces.cpp). PrintStatusWidget's idle runout dialog
 * was the one left over: its "Load filament" button called
 * set_active(PanelId::Filament), which navigated the dashboard away and left the
 * user to find the Load button themselves — on a printer with no AMS and no
 * LOAD_FILAMENT macro, to press a button that would then fall back to raw gcode
 * anyway.
 *
 * This is the idle runout (nothing printing), so it carries the same
 * ParamPolicy::Suppress constraint as FilamentRunoutHandler: a MacroParamModal
 * raised on top of the live guidance dialog would stack on a modal whose own
 * observers keep firing underneath it.
 */

#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_status_widget_test_access.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_op_router.h"
#include "macro_param_cache.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "standard_macros.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::CachedMacroInfo;
using helix::MacroExecuteCallback;
using helix::PrintStatusWidget;
using helix::PrintStatusWidgetTestAccess;

namespace {

/// Gcode the tier-3 load fallback must contain (filament_load_fallback_gcode()).
constexpr const char* LOAD_FALLBACK_MARKER = "G1 E56";

bool s_widget_subjects_ready = false;

class IdleRunoutDispatchFixture : public LVGLTestFixture {
  public:
    IdleRunoutDispatchFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        if (!s_widget_subjects_ready) {
            helix::PanelWidgetManager::instance().init_widget_subjects();
            s_widget_subjects_ready = true;
        }
        // PrintStatusWidget's static-inline subjects come from its own ctor, not
        // from init_widget_subjects() — the "print_status" registry row carries a
        // null init_subjects hook. Idempotent.
        PrintStatusWidget::init_static_subjects();

        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());

        // No AMS backend: the shape a basic runout-sensor printer has, and the
        // one the old code answered by navigating away.
        AmsState::instance().clear_backends();
        // Empty cache => MacroParamKnowledge::UNKNOWN, i.e. the branch that WOULD
        // prompt. The only way to prove Suppress suppresses something.
        helix::MacroParamCache::instance().clear();

        helix::ui::set_filament_param_prompter([this](const std::string& macro,
                                                      const CachedMacroInfo&,
                                                      MacroExecuteCallback on_execute) {
            ++prompt_count;
            prompted_macro = macro;
            pending_execute = std::move(on_execute);
        });
    }

    ~IdleRunoutDispatchFixture() override {
        helix::ui::set_filament_param_prompter({});
        set_moonraker_api(previous_api_);
        StandardMacros::instance().reset();
        helix::MacroParamCache::instance().clear();
        AmsState::instance().clear_backends();
        helix::ui::UpdateQueue::instance().drain();
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    void configure_filament_macros() {
        helix::PrinterDiscovery hardware;
        nlohmann::json objects = {"extruder", "gcode_macro LOAD_FILAMENT"};
        hardware.parse_objects(objects);
        StandardMacros::instance().reset();
        StandardMacros::instance().init(hardware);
        REQUIRE_FALSE(StandardMacros::instance().get(StandardMacroSlot::LoadFilament).is_empty());
    }

    void clear_filament_macros() {
        StandardMacros::instance().reset();
        REQUIRE(StandardMacros::instance().get(StandardMacroSlot::LoadFilament).is_empty());
    }

    [[nodiscard]] bool gcode_sent_containing(const std::string& needle) const {
        for (const auto& script : mock_client.gcode_script_history()) {
            if (script.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;

    int prompt_count = 0;
    std::string prompted_macro;
    MacroExecuteCallback pending_execute;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(IdleRunoutDispatchFixture,
                 "Idle runout load dispatches the macro instead of navigating away",
                 "[print_status_widget][dispatch][runout]") {
    configure_filament_macros();
    REQUIRE(AmsState::instance().get_backend() == nullptr);

    const helix::PanelId before = NavigationManager::instance().get_active();

    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::dispatch_load(widget);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(gcode_sent_containing("LOAD_FILAMENT"));
    // The old arm was set_active(PanelId::Filament) — it tore the dashboard out
    // from under the dialog the user was standing in.
    CHECK(NavigationManager::instance().get_active() == before);
}

TEST_CASE_METHOD(IdleRunoutDispatchFixture, "Idle runout load never prompts for macro parameters",
                 "[print_status_widget][dispatch][runout][params]") {
    // The macro's params are UNKNOWN here, which is exactly the state that makes
    // the Filament panel raise MacroParamModal. Stacking that on the live
    // guidance dialog is what ParamPolicy::Suppress exists to prevent.
    configure_filament_macros();

    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::dispatch_load(widget);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(prompt_count == 0);
    CHECK(gcode_sent_containing("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(IdleRunoutDispatchFixture,
                 "Idle runout load with no backend and no macro falls back to raw gcode",
                 "[print_status_widget][dispatch][runout]") {
    clear_filament_macros();

    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::dispatch_load(widget);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(prompt_count == 0);
    CHECK(gcode_sent_containing(LOAD_FALLBACK_MARKER));
}
