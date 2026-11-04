// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_probe_config_edit_failure.cpp
 * @brief A failed probe config edit reaches the user, not only the log.
 *
 * Run with: ./build/bin/helix-tests "[probe][config_edit]"
 *
 * The edit modal closes as soon as Save is tapped, so the safe-edit flow's
 * failure callback is the only place left to say that nothing was written.
 * An error-level log line reaches no one at the screen (prestonbrown/helixscreen#1373).
 */

#include "ui_probe_overlay.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/probe_overlay_test_access.h"
#include "../ui_test_utils.h"
#include "core/update_queue.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

class ProbeEditFixture : public LVGLTestFixture {
  public:
    ProbeEditFixture() : api_(client_, state_) {
        helix::ui::set_test_notification_error_hook(
            [this](const std::string& message) { errors_.push_back(message); });
        overlay_.set_api(&api_);
    }

    ~ProbeEditFixture() override {
        wait_for_errors(1);
        helix::ui::set_test_notification_error_hook(nullptr);
        overlay_.set_api(nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// The edit runs on a worker and reports back through the UI queue.
    bool wait_for_errors(size_t n) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            helix::ui::UpdateQueue::instance().drain();
            if (errors_.size() >= n) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return errors_.size() >= n;
    }

    MoonrakerClientMock client_{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state_;
    MoonrakerAPIMock api_;
    ProbeOverlay overlay_;
    std::vector<std::string> errors_;
};

} // namespace

TEST_CASE_METHOD(ProbeEditFixture, "a probe config edit that cannot be applied tells the user",
                 "[probe][config_edit][1373]") {
    // Nothing seeded: the config load itself fails before any edit is attempted.
    ProbeOverlayTestAccess::stage_edit(overlay_, "probe", "z_offset", "1.250");
    ProbeOverlayTestAccess::save(overlay_);

    REQUIRE(wait_for_errors(1));
    CHECK(errors_[0].find("z_offset") != std::string::npos);
}
