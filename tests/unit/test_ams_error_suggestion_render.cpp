// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_error_suggestion_render.cpp
 * @brief AmsError::suggestion has to reach the screen.
 *
 * Every backend populated `suggestion` and nothing rendered it: a grep for
 * `.suggestion` across src/ and include/ found no hit outside the WiFi code.
 * Thirty-odd call sites each open-coded `NOTIFY_ERROR(..., err.user_msg)` with
 * their own verb prefix, so a runout-paused user read "Cannot run filament
 * operation while printing" and never "Pause the print first, then load,
 * unload, or change filament" — the only half that tells them what to do.
 *
 * These tests assert the rendered OUTPUT, not that a function exists: the
 * notification hooks (tests/ui_test_utils.h) receive the notification-history
 * row the call adds, which joins the toast's two lines.
 */

#include "ui_error_reporting.h"

#include "../helix_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_error.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsError;
using helix::AmsErrorHelper;
using helix::AmsResult;

namespace {

struct AmsErrorRenderFixture : public HelixTestFixture {
    AmsErrorRenderFixture() {
        helix::ui::set_test_notification_error_hook(
            [this](const std::string& msg) { errors.push_back(msg); });
        helix::ui::set_test_notification_warning_hook(
            [this](const std::string& msg) { warnings.push_back(msg); });
    }

    ~AmsErrorRenderFixture() override {
        helix::ui::set_test_notification_error_hook(nullptr);
        helix::ui::set_test_notification_warning_hook(nullptr);
    }

    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

} // namespace

// The regression itself

TEST_CASE_METHOD(AmsErrorRenderFixture, "a rendered AmsError carries its suggestion",
                 "[ams][error-render]") {
    // The exact refusal a user hits when they tap Load on a printing job.
    const AmsError err = AmsErrorHelper::print_active(/*is_paused=*/false,
                                                      /*pause_allows_ops=*/true);
    REQUIRE_FALSE(err.suggestion.empty());

    helix::ui::notify_ams_error(err);

    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("Cannot run filament operation while printing") != std::string::npos);
    CHECK(errors[0].find("Pause the print first, then load, unload, or change filament") !=
          std::string::npos);
}

TEST_CASE_METHOD(AmsErrorRenderFixture, "the longest suggestion in the tree survives rendering",
                 "[ams][error-render]") {
    // print_active(paused) is the worst case: a 44-character message and a
    // 96-character suggestion. Nothing may truncate it.
    const AmsError err = AmsErrorHelper::print_active(/*is_paused=*/true);

    helix::ui::notify_ams_error(err);

    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find(err.user_msg) != std::string::npos);
    CHECK(errors[0].find(err.suggestion) != std::string::npos);
}

TEST_CASE_METHOD(AmsErrorRenderFixture, "warnings render the suggestion too",
                 "[ams][error-render]") {
    // The filament surfaces' pre-guards refuse before dispatching and report at
    // warning severity — they must not be the path that loses the suggestion.
    helix::ui::notify_ams_warning(AmsErrorHelper::print_active(false, true));

    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("Pause the print first") != std::string::npos);
    CHECK(errors.empty());
}

// Composition rules

TEST_CASE_METHOD(AmsErrorRenderFixture, "a context word prefixes the message, never the suggestion",
                 "[ams][error-render]") {
    // command_failed()'s user_msg names no operation ("Command failed"), which
    // is the case a context word exists for.
    const AmsError err = AmsErrorHelper::command_failed("AMS_HOME", "timeout");

    helix::ui::notify_ams_error(err, "Home failed");

    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("Home failed: Command failed") != std::string::npos);
    CHECK(errors[0].find("Check Klipper console for details") != std::string::npos);
    // The suggestion is a separate line, so the context must not have been
    // spliced in front of it as well.
    CHECK(errors[0].find("Home failed: Check Klipper") == std::string::npos);
}

TEST_CASE_METHOD(AmsErrorRenderFixture, "an error with no suggestion renders just the message",
                 "[ams][error-render]") {
    const AmsError err(AmsResult::LOAD_FAILED, "tech", "Something went wrong");

    helix::ui::notify_ams_error(err);

    REQUIRE(errors.size() == 1);
    CHECK(errors[0] == "Something went wrong");
}

TEST_CASE_METHOD(AmsErrorRenderFixture, "an empty AmsError still says something",
                 "[ams][error-render]") {
    // A backend that populated neither user-facing field must not produce a
    // blank toast; the result name is the floor.
    helix::ui::notify_ams_error(AmsError(AmsResult::TIMEOUT));

    REQUIRE(errors.size() == 1);
    CHECK(errors[0] == "Timeout");
}

TEST_CASE_METHOD(AmsErrorRenderFixture, "a successful AmsError reports nothing",
                 "[ams][error-render]") {
    helix::ui::notify_ams_error(AmsErrorHelper::success());
    helix::ui::notify_ams_warning(AmsErrorHelper::success());

    CHECK(errors.empty());
    CHECK(warnings.empty());
}

// display_text() — the flattened form, for boundaries that carry one string

TEST_CASE("AmsError::display_text joins both user-facing halves", "[ams][error-render]") {
    SECTION("both present") {
        const AmsError err(AmsResult::UNLOAD_FAILED, "tech", "Failed to unload filament",
                           "Check extruder temperature and try again");
        const std::string text = err.display_text();
        CHECK(text.find("Failed to unload filament") != std::string::npos);
        CHECK(text.find("Check extruder temperature and try again") != std::string::npos);
    }
    SECTION("suggestion only") {
        const AmsError err(AmsResult::UNLOAD_FAILED, "tech", "", "Retry the unload");
        CHECK(err.display_text() == "Retry the unload");
    }
    SECTION("message only") {
        const AmsError err(AmsResult::UNLOAD_FAILED, "tech", "Failed to unload filament");
        CHECK(err.display_text() == "Failed to unload filament");
    }
    SECTION("neither — the result name is the floor") {
        CHECK(AmsError(AmsResult::FILAMENT_JAM).display_text() == "Filament Jam");
    }
}
