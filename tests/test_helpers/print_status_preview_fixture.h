// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A PrintStatusPanel wired to a real G-code viewer and a mock API whose
// downloads are held until the test releases them by name, so overlapping
// fetches can be made to land in any order. Shared by the print-status tests
// that drive the preview fetch.

#include "ui_gcode_viewer.h"
#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/delayed_file_transfers.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "../test_helpers/scoped_cache_dir.h"
#include "../test_helpers/update_queue_test_access.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace print_status_preview_test {

using namespace helix;

class PrintStatusPreviewFixture : public LVGLTestFixture {
  public:
    PrintStatusPreviewFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24) {
        // register_xml=false keeps these subjects out of the process-wide XML
        // registry, which outlives this fixture.
        state_.init_subjects(false);
        // Every download parks until the test completes it by path.
        transfers_.hold_transfers = true;
        api_ = std::make_unique<DelayedMoonrakerAPIMock>(client_, state_, transfers_);
        panel_ = std::make_unique<PrintStatusPanel>(state_, api_.get());

        // The streaming completion path sizes its 2D renderer from the widget's
        // coords, so the viewer gets a real size.
        viewer_ = helix::ui::gcode_viewer_create(test_screen());
        REQUIRE(viewer_ != nullptr);
        lv_obj_set_size(viewer_, 240, 240);
        lv_obj_update_layout(viewer_);
        PrintStatusPanelTestAccess::set_gcode_viewer(*panel_, viewer_);
    }

    ~PrintStatusPreviewFixture() override {
        // The viewer's load callback points at the panel, so the viewer goes first.
        PrintStatusPanelTestAccess::set_gcode_viewer(*panel_, nullptr);
        helix::ui::gcode_viewer_clear(viewer_);
        lv_obj_delete(viewer_);
        process_lvgl(50);
        panel_.reset();
        helix::ui::UpdateQueue::instance().drain();
        state_.deinit_subjects();
    }

    /// Report @p filename as the printing file the way Moonraker does, so the
    /// print's identity is decided where production decides it.
    void report_print(const std::string& filename) {
        nlohmann::json status = {{"print_stats", {{"filename", filename}}}};
        state_.update_from_status(status);
        drain();
    }

    /// Start fetching @p filename for the viewer and run the fetch up to its
    /// download, which the transfer mock holds.
    void start_fetch(const std::string& filename) {
        const size_t held_before = transfers_.held_count();
        PrintStatusPanelTestAccess::load_gcode_for_viewing(*panel_, filename);
        drain();
        REQUIRE(transfers_.held_count() == held_before + 1);
    }

    /// Complete @p filename's download and wait for the viewer load it starts
    /// to reach the panel's load callback, which publishes the scan. The
    /// callback also defers state writes through the UpdateQueue, so the queue
    /// is drained before anything is read back.
    void land(const std::string& filename) {
        const int version = pause_markers_version();
        REQUIRE(transfers_.release(filename));
        REQUIRE(wait_until([&] { return pause_markers_version() > version; }, 30000));
        drain();
    }

    /// Complete @p filename's held download for a load the panel is expected
    /// to drop as no longer the effective print. A dropped load never reaches
    /// the viewer and never starts a background build, so there is no publish
    /// to wait on: releasing the download and draining the queue that carries
    /// it to load_gcode_file() is the whole round trip in the passing case.
    /// The extra settle wait matters only when a gate under test has been
    /// removed by hand: it gives a load that slipped past it time to finish
    /// its background build and deliver, so the assertions that follow see
    /// its result rather than a work-in-progress false negative.
    void land_dropped(const std::string& filename) {
        REQUIRE(transfers_.release(filename));
        drain();
        // A load that slipped past the entry gate (a temporary revert, when
        // proving that gate is load-bearing) starts a real background build,
        // and the very first one run against a fresh viewer can take well
        // over a second before it is queued for delivery. Waiting on the
        // queue rather than a fixed sleep is what makes this reliable either
        // way; the drain that follows is what actually delivers it, which is
        // also where the load callback's own identity check - still present
        // even with the entry gate reverted - routes a stale delivery through
        // ensure_preview_current() and clears whatever it just installed.
        wait_for_queued_result(std::chrono::seconds(5));
        drain();
    }

    /// Wait on the real clock, without draining the UpdateQueue, until a
    /// worker has queued something. Lets a test observe a load's background
    /// build finishing before the result is delivered to the panel.
    bool wait_for_queued_result(std::chrono::milliseconds budget) {
        auto& queue = helix::ui::UpdateQueue::instance();
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!helix::ui::UpdateQueueTestAccess::queue_empty(queue)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return !helix::ui::UpdateQueueTestAccess::queue_empty(queue);
    }

    int pause_markers_version() {
        return lv_subject_get_int(state_.print_state().get_pause_markers_version_subject());
    }

    const std::string& gcode_displayed_file() const {
        return PrintStatusPanelTestAccess::gcode_displayed_file(*panel_);
    }

    void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    // A cache directory of this fixture's own. A copy left by an earlier load of
    // the same file takes the panel's cached-file branch, which loads without any
    // download and so never overlaps with anything.
    helix::test::ScopedCacheDir cache_;
    MoonrakerClientMock client_;
    PrinterState state_;
    DelayedFileTransfers transfers_{client_, ""};
    std::unique_ptr<MoonrakerAPIMock> api_;
    std::unique_ptr<PrintStatusPanel> panel_;
    lv_obj_t* viewer_ = nullptr;
};

} // namespace print_status_preview_test
