// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_login_refused.cpp
 * @brief A file list that fails because Moonraker refused the login raises no
 *        refresh error of its own: the refusal already says why.
 */

#include "ui_panel_print_select.h"

#include "../test_helpers/print_select_panel_fixture.h"
#include "../ui_test_utils.h"
#include "moonraker_auth.h"
#include "moonraker_error.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class LoginRefusedListFixture : private PrintSelectGlobalStateReset,
                                public PrintSelectPanelFixture {
  public:
    LoginRefusedListFixture() {
        helix::ui::set_test_notification_event_hook(
            [errors = errors_](const helix::ui::NotificationEvent& e) {
                if (e.severity == ToastSeverity::ERROR) {
                    errors->push_back(e.message);
                }
            });
    }

    ~LoginRefusedListFixture() override {
        helix::ui::set_test_notification_event_hook(nullptr);
        moonraker_auth::set_credential({});
    }

    /// Fails the next directory listing the way a refused login does, then refreshes.
    void refresh_with_failed_listing() {
        mock_client_.fail_next("server.files.get_directory",
                               MoonrakerError::unknown("Unauthorized"));
        panel_->refresh_files(/*force=*/true);
        drain();
        REQUIRE(mock_client_.call_count("server.files.get_directory") > 0);
    }

    std::shared_ptr<std::vector<std::string>> errors_ =
        std::make_shared<std::vector<std::string>>();
};

} // namespace

TEST_CASE_METHOD(LoginRefusedListFixture, "Print select: a failed listing reports a refresh error",
                 "[print_select][api-key]") {
    errors_->clear();
    refresh_with_failed_listing();
    CHECK(*errors_ == std::vector<std::string>{"Failed to refresh file list"});
}

TEST_CASE_METHOD(LoginRefusedListFixture,
                 "Print select: a listing failed by a refused login adds no refresh error",
                 "[print_select][api-key]") {
    moonraker_auth::set_credential({"printer.lan", 7125, ""});
    moonraker_auth::mark_unauthorized();
    REQUIRE(moonraker_auth::unauthorized());

    errors_->clear();
    refresh_with_failed_listing();
    CHECK(errors_->empty());
}
