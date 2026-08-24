// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_action_prompt_gcode_failure.cpp
 * @brief A prompt button whose macro aborts must tell the user something
 *
 * The ActionPromptModal closes on every button press by design and waits for
 * the firmware to push a replacement prompt. When the macro fails there is no
 * replacement, and the RPC layer will not fill the gap either: supplying an
 * error callback marks the call caller-handled in
 * MoonrakerRequestTracker::route_response(), which records the message in
 * rpc_error_correlation and suppresses the independent `!!` GcodeError toast
 * for the same failure. Before this, the dialog simply vanished.
 */

#include "../helix_test_fixture.h"
#include "../ui_test_utils.h"
#include "action_prompt_modal.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

/// Captures what the notification layer was asked to surface. User-facing
/// toasts are stubbed out of the test build, so the hook the stub calls is the
/// only observation point (tests/ui_test_utils.h).
class ErrorNotificationWatcher {
  public:
    ErrorNotificationWatcher() {
        helix::ui::set_test_notification_error_hook(
            [this](const std::string& msg) { messages_.push_back(msg); });
    }
    ~ErrorNotificationWatcher() {
        helix::ui::set_test_notification_error_hook(nullptr);
    }

    [[nodiscard]] const std::vector<std::string>& messages() const {
        return messages_;
    }

  private:
    std::vector<std::string> messages_;
};

} // namespace

TEST_CASE_METHOD(HelixTestFixture, "Action prompt gcode failure is surfaced to the user",
                 "[action_prompt][gcode_failure]") {
    ErrorNotificationWatcher watcher;

    report_action_prompt_gcode_failure("Extruder not hot enough");

    REQUIRE(watcher.messages().size() == 1);
    // Klipper's own wording is the whole value of the message: "something
    // failed" tells the user nothing they cannot already see.
    CHECK(watcher.messages()[0].find("Extruder not hot enough") != std::string::npos);
}

TEST_CASE_METHOD(HelixTestFixture, "Action prompt gcode failure message is never blank",
                 "[action_prompt][gcode_failure]") {
    ErrorNotificationWatcher watcher;

    // A timeout or a dropped connection can leave the error body empty.
    report_action_prompt_gcode_failure("");

    REQUIRE(watcher.messages().size() == 1);
    CHECK_FALSE(watcher.messages()[0].empty());
    // Not just the bare "Macro failed: " prefix with nothing after it.
    CHECK(watcher.messages()[0].back() != ' ');
}
