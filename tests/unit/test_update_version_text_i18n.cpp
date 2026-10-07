// SPDX-License-Identifier: GPL-3.0-or-later

// update_version_text is a subject string, so nothing re-translates it the way
// LVGL re-translates a label: it has to be re-rendered from the last check's
// result when the language changes (prestonbrown/helixscreen#1023).

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/update_checker_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "system/update_checker.h"
#include "system_settings_manager.h"
#include "version.h"

#include <spdlog/fmt/fmt.h>

#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::SystemSettingsManager;

namespace {

std::string version_text(UpdateChecker& c) {
    return lv_subject_get_string(c.version_text_subject());
}

void land(UpdateChecker& c, UpdateChecker::Status status,
          std::optional<UpdateChecker::ReleaseInfo> info = std::nullopt,
          const std::string& error = "") {
    UpdateCheckerTestAccess::report_result(c, status, std::move(info), error);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

UpdateChecker::ReleaseInfo release(const std::string& version, bool downgrade = false) {
    UpdateChecker::ReleaseInfo info;
    info.version = version;
    info.is_downgrade = downgrade;
    return info;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "update_version_text follows a runtime language change",
                 "[update_checker][subjects][i18n]") {
    auto& checker = UpdateChecker::instance();
    checker.clear_cache();
    checker.init();
    auto& settings = SystemSettingsManager::instance();
    settings.init_subjects();
    settings.set_language("en");

    SECTION("up to date") {
        land(checker, UpdateChecker::Status::UpToDate);
        REQUIRE(version_text(checker) == "Up to date");
        settings.set_language("de");
        CHECK(version_text(checker) == "Aktuell");
    }

    SECTION("update available") {
        land(checker, UpdateChecker::Status::UpdateAvailable, release("9.9.9"));
        REQUIRE(version_text(checker) == "v9.9.9 available");
        settings.set_language("de");
        CHECK(version_text(checker) == "v9.9.9 verfügbar");
    }

    SECTION("downgrade") {
        land(checker, UpdateChecker::Status::UpdateAvailable, release("1.0.0", true));
        REQUIRE(version_text(checker) == "Switch to v1.0.0");
        settings.set_language("de");
        CHECK(version_text(checker) == "Zu v1.0.0 wechseln");
    }

    SECTION("error") {
        land(checker, UpdateChecker::Status::Error, std::nullopt, "timeout");
        REQUIRE(version_text(checker) == "Error: timeout");
        settings.set_language("de");
        CHECK(version_text(checker) == "Fehler: timeout");
    }

    SECTION("no check yet keeps the installed version") {
        settings.set_language("de");
        CHECK(version_text(checker) == fmt::format("Version {}", HELIX_VERSION));
    }

    settings.set_language("en");
    checker.clear_cache();
    checker.shutdown();
}
