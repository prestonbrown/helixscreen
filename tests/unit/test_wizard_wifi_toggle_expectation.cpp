// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wizard_wifi_toggle_expectation.cpp
 * @brief A wizard tap must not record a radio state nobody has confirmed.
 *
 * The stored WiFi expectation is what a later boot believes this printer was
 * set up with, and the wizard saves its config when the user finishes. A tap on
 * the switch knows only what was asked for: driving the radio takes seconds and
 * can fail, so the value belongs to the answer, written by the manager that
 * heard it.
 *
 * The two halves are told apart by timing alone, which is the point: the tap is
 * synchronous, the answer arrives through the UpdateQueue. A value that appears
 * before anything is drained can only have been written by the tap.
 */

#include "ui_update_queue.h"
#include "ui_wizard_wifi.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/wizard_wifi_test_access.h"
#include "config.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <chrono>
#include <memory>
#include <thread>

#include "../catch_amalgamated.hpp"

using Access = WizardWifiStepTestAccess;

namespace {

class WizardWifiToggleFixture : public LVGLUITestFixture {
  protected:
    ScopedRuntimeConfig scoped_config;

    /// A step holding a mock-backed manager. init_wifi_manager() would take the
    /// process-wide one and build an EthernetManager with it; the toggle path
    /// needs neither, and no screen — it reads its state off the switch it is
    /// handed.
    WizardWifiStep& step_with_manager() {
        if (!started_) {
            step_.init_subjects();
            get_runtime_config()->test_mode = true;
            get_runtime_config()->use_real_wifi = false;
            auto backend = std::make_unique<WifiBackendMock>();
            REQUIRE(backend->start().success());
            manager_ = std::make_shared<helix::WiFiManager>(std::move(backend), /*silent=*/true);
            manager_->init_self_reference(manager_);
            Access::wifi_manager(step_) = manager_;
            // The mock fires READY from the constructor, which queues the
            // stored-radio-state reassert. Run it now so it cannot land in the
            // middle of an assertion below.
            helix::ui::UpdateQueue::instance().drain();
            started_ = true;
        }
        return step_;
    }

    lv_obj_t* switch_widget() {
        lv_obj_t* toggle = lv_obj_create(test_screen());
        REQUIRE(toggle != nullptr);
        return toggle;
    }

    /// Spins the queue on this thread until the stored expectation reads
    /// `want`; the radio answers from an HttpExecutor lane, so there is no
    /// handle to join.
    static bool wait_for_expectation(Config* config, bool want, int timeout_ms = 5000) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            helix::ui::UpdateQueue::instance().drain();
            if (config->is_wifi_expected() == want) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        helix::ui::UpdateQueue::instance().drain();
        return config->is_wifi_expected() == want;
    }

  private:
    WizardWifiStep step_;
    std::shared_ptr<helix::WiFiManager> manager_;
    bool started_ = false;
};

} // namespace

TEST_CASE_METHOD(WizardWifiToggleFixture,
                 "A wizard WiFi tap stores nothing until the radio answers",
                 "[wizard][wifi][radio]") {
    Config* config = Config::get_instance();
    REQUIRE(config != nullptr);

    WizardWifiStep& step = step_with_manager();
    config->set_wifi_expected(false);

    // Tapped ON deliberately: the off branch clears the network list, which
    // drains the queue itself and would let the answer land before the check
    // below could tell the two writers apart.
    Access::toggle_changed(step, switch_widget(), /*checked=*/true);

    // Nothing has been drained, so the answer cannot have arrived: anything
    // stored at this point was stored by the tap.
    CHECK_FALSE(config->is_wifi_expected());

    // Now let it answer. The radio went on, and that is what gets recorded —
    // without this half, the check above would hold just as well against a
    // toggle nobody ever records.
    REQUIRE(wait_for_expectation(config, true));
    CHECK(config->is_wifi_expected());

    helix::ui::UpdateQueue::instance().drain();
}
