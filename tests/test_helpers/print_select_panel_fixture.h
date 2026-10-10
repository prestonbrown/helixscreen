// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file print_select_panel_fixture.h
 * @brief Shared PrintSelectPanel integration fixture and gcode-file planter.
 *
 * The delete-guard and filelist-notification suites both drive the real panel
 * over a connected MoonrakerClientMock, and both need a real .gcode on disk for
 * the mock's gcodes root to report. Two parts of the setup are load-bearing and
 * easy to get subtly wrong: drain() has to join the ThumbnailProcessor pool
 * between UpdateQueue passes, and the destructor has to drop the panel's
 * NavigationManager registration before the panel dies. One copy of each is
 * what keeps the suites from disagreeing about either.
 */

#include "ui_nav_manager.h"
#include "ui_panel_print_select.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "planted_gcode.h"
#include "print_state_test_drivers.h"
#include "printer_state.h"
#include "printer_state_test_access.h"
#include "thumbnail_processor.h"
#include "update_queue_test_access.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

namespace helix {

/// Resets the shard-global PrinterState BEFORE a panel fixture builds the real
/// panel over it (list it as a private base constructed first). Without this,
/// a prior case's subjects or preparing job decide this one's answers.
struct PrintSelectGlobalStateReset {
    PrintSelectGlobalStateReset() {
        auto& ps = get_printer_state();
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        if (ps.print_state().has_preparing_job()) {
            ps.print_state().retire_preparing(helix::PreparingExit::Superseded);
        }
        helix::test::set_wire_state(ps, PrintJobState::STANDBY);
    }
};

/// Whether the fixture hands the panel its API again after setup().
///
/// PrintSelectPanel::set_api() is what registers the notify_filelist_changed
/// handler and starts the poll timer, so a suite that fires notifications needs
/// it. It also issues an early refresh, which a suite driving the list by hand
/// does not want.
enum class PrintSelectFilelistHandler { Unregistered, Registered };

/// Whether the fixture navigates to the panel (first on_activate) at the end of
/// construction. Deferred leaves the panel set up but never visited.
enum class PrintSelectVisit { Immediate, Deferred };

/// Which API the panel talks to. Mock is MoonrakerAPIMock, whose transfers
/// record uploads instead of needing an HTTP server.
enum class PrintSelectApi { Real, Mock };

/// What the connection-state subject says when setup() registers the panel's
/// observer. Connected is a first visit on a platform that builds the panel on
/// demand; Unset leaves whatever the subjects were initialized to.
enum class PrintSelectConnectionAtSetup { Unset, Connected };

/// The real panel over the real NavigationManager: mock client connected,
/// MoonrakerAPI on top of it, print_select_panel XML built, and the navigation
/// stack seeded the way the app has it (panel_stack_[0] = the active main
/// panel). Animations off — the deterministic go_back path then runs inline in
/// the drain instead of an animation completion tick later.
class PrintSelectPanelFixture : public LVGLUITestFixture {
  public:
    explicit PrintSelectPanelFixture(
        PrintSelectFilelistHandler handler = PrintSelectFilelistHandler::Unregistered,
        PrintSelectVisit visit = PrintSelectVisit::Immediate,
        PrintSelectApi api = PrintSelectApi::Real,
        PrintSelectConnectionAtSetup connection = PrintSelectConnectionAtSetup::Unset)
        : mock_client_(MoonrakerClientMock::PrinterType::VORON_24, /*speedup_factor=*/100.0) {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        // Connect before the API exists, like IdleRunoutGraceFixture: the
        // initial-state dispatch then has no subscribers to storm, while
        // get_connection_state() still reports CONNECTED for is_ready() checks.
        mock_client_.connect("ws://mock/websocket", []() {}, []() {});
        if (api == PrintSelectApi::Mock) {
            api_ = std::make_unique<MoonrakerAPIMock>(mock_client_, get_printer_state());
        } else {
            api_ = std::make_unique<MoonrakerAPI>(mock_client_, get_printer_state());
        }

        panel_ = std::make_unique<PrintSelectPanel>(get_printer_state(), api_.get());
        panel_->init_subjects();
        if (connection == PrintSelectConnectionAtSetup::Connected) {
            lv_subject_set_int(
                get_printer_state().network_state().get_printer_connection_state_subject(),
                static_cast<int>(ConnectionState::CONNECTED));
        }
        panel_obj_ =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "print_select_panel", nullptr));
        REQUIRE(panel_obj_ != nullptr);
        panel_->setup(panel_obj_, test_screen());
        if (handler == PrintSelectFilelistHandler::Registered) {
            // setup() creates file_provider_; the app then hands the panel its
            // API, which is what registers the notification handler.
            panel_->set_api(api_.get());
        }

        home_widget_ = lv_obj_create(test_screen());
        lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_widget_;
        panels[static_cast<int>(PanelId::PrintSelect)] = panel_obj_;
        auto& nav = NavigationManager::instance();
        nav.set_panels(panels);
        nav.register_panel_instance(PanelId::PrintSelect, panel_.get());
        if (visit == PrintSelectVisit::Immediate) {
            nav.set_active(PanelId::PrintSelect);
            drain();
        }
    }

    ~PrintSelectPanelFixture() override {
        auto& nav = NavigationManager::instance();
        // Drop the panel registration BEFORE the panel dies: NavigationManager
        // keeps raw panel pointers, and the base fixture still walks it.
        nav.register_panel_instance(PanelId::PrintSelect, nullptr);
        drain();
        panel_.reset();
        api_.reset();
        mock_client_.stop_temperature_simulation();
        mock_client_.disconnect();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    /// Every hop in a delete or refresh chain crosses the UpdateQueue
    /// (token.defer, queue_update, go_back's own queue_update, the refresh's
    /// on_files_ready); drain until fully empty like OverlayActivationFixture
    /// does. Those chains also land work on the ThumbnailProcessor pool, which
    /// the queue-only drain never waits for — the worker would outlive the test
    /// holding callbacks into its state (ISOLATION-LEAK, then a UAF crash in a
    /// later test). Join the pool between queue passes the way
    /// ActivePrintMediaAsyncFixture::drain() does.
    static void drain() {
        auto& processor = helix::ThumbnailProcessor::instance();
        auto& queue = helix::ui::UpdateQueue::instance();
        for (int pass = 0; pass < 4; ++pass) {
            processor.wait_for_completion();
            if (processor.pending_tasks() == 0 &&
                helix::ui::UpdateQueueTestAccess::queue_empty(queue)) {
                break;
            }
            helix::ui::UpdateQueueTestAccess::drain_all(queue);
        }
    }

    MoonrakerClientMock mock_client_;
    std::unique_ptr<MoonrakerAPI> api_;
    std::unique_ptr<PrintSelectPanel> panel_;
    lv_obj_t* panel_obj_ = nullptr;
    lv_obj_t* home_widget_ = nullptr;
    bool animations_were_enabled_ = true;
};

} // namespace helix
