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
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "thumbnail_processor.h"
#include "update_queue_test_access.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

namespace helix {

/// A .gcode planted in the mock's virtual gcodes root for one test.
///
/// MoonrakerClientMock backs the gcodes root with assets/test_gcodes on disk
/// (scan_mock_gcode_files() is a real directory scan), so what is on disk is
/// what the next get_directory reports: a delete driven through the mock
/// removes the real file, and remove_from_disk() stands in for an operation
/// that already happened on the printer's storage. Plant one nothing else
/// depends on and take it back out however the test ends.
class PlantedGcode {
  public:
    explicit PlantedGcode(const std::string& name) {
        for (const auto* prefix : {"", "../", "../../"}) {
            std::string dir = std::string(prefix) + "assets/test_gcodes";
            if (std::filesystem::is_directory(dir)) {
                path_ = dir + "/" + name;
                break;
            }
        }
        REQUIRE_FALSE(path_.empty());
        std::ofstream out(path_, std::ios::trunc);
        out << "; planted for a print-select test\nG28\n";
    }

    ~PlantedGcode() {
        std::remove(path_.c_str());
    }

    PlantedGcode(const PlantedGcode&) = delete;
    PlantedGcode& operator=(const PlantedGcode&) = delete;

    bool on_disk() const {
        return std::filesystem::exists(path_);
    }

    /// Take the file off disk behind the panel's back, so only a notification
    /// can tell it the listing is stale. The dtor's remove() tolerates the file
    /// already being gone.
    bool remove_from_disk() {
        return std::remove(path_.c_str()) == 0;
    }

    std::string name() const {
        return std::filesystem::path(path_).filename().string();
    }

  private:
    std::string path_;
};

/// Whether the fixture hands the panel its API again after setup().
///
/// PrintSelectPanel::set_api() is what registers the notify_filelist_changed
/// handler and starts the poll timer, so a suite that fires notifications needs
/// it. It also issues an early refresh, which a suite driving the list by hand
/// does not want.
enum class PrintSelectFilelistHandler { Unregistered, Registered };

/// The real panel over the real NavigationManager: mock client connected,
/// MoonrakerAPI on top of it, print_select_panel XML built, and the navigation
/// stack seeded the way the app has it (panel_stack_[0] = the active main
/// panel). Animations off — the deterministic go_back path then runs inline in
/// the drain instead of an animation completion tick later.
class PrintSelectPanelFixture : public LVGLUITestFixture {
  public:
    explicit PrintSelectPanelFixture(
        PrintSelectFilelistHandler handler = PrintSelectFilelistHandler::Unregistered)
        : mock_client_(MoonrakerClientMock::PrinterType::VORON_24, /*speedup_factor=*/100.0) {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        // Connect before the API exists, like IdleRunoutGraceFixture: the
        // initial-state dispatch then has no subscribers to storm, while
        // get_connection_state() still reports CONNECTED for is_ready() checks.
        mock_client_.connect("ws://mock/websocket", []() {}, []() {});
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, get_printer_state());

        panel_ = std::make_unique<PrintSelectPanel>(get_printer_state(), api_.get());
        panel_->init_subjects();
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
        nav.set_active(PanelId::PrintSelect);
        drain();
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
