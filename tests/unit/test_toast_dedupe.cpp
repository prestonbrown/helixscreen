// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_toast_manager.h"

#include "../catch_amalgamated.hpp"

// Friend accessor (L065): reach ToastManager privates without test-only
// methods on the production class.
class ToastManagerTestAccess {
  public:
    static void inject(ToastManager& tm, ToastSeverity sev, const char* msg, bool exiting) {
        ToastManager::ToastInstance inst;
        inst.severity = sev;
        inst.message = msg;
        inst.is_exiting = exiting;
        tm.active_.push_back(std::move(inst));
    }
    static bool refresh_duplicate(ToastManager& tm, ToastSeverity sev, const char* msg) {
        return tm.refresh_duplicate(sev, msg);
    }
    static void clear(ToastManager& tm) {
        tm.active_.clear();
    }
};

TEST_CASE("Toast dedupe: identical active toast is refreshed, not duplicated", "[toast][dedupe]") {
    auto& tm = ToastManager::instance();
    ToastManagerTestAccess::clear(tm);

    ToastManagerTestAccess::inject(tm, ToastSeverity::ERROR, "Jog failed: busy", false);
    CHECK(ToastManagerTestAccess::refresh_duplicate(tm, ToastSeverity::ERROR, "Jog failed: busy"));

    ToastManagerTestAccess::clear(tm);
}

TEST_CASE("Toast dedupe: different message or severity does not match", "[toast][dedupe]") {
    auto& tm = ToastManager::instance();
    ToastManagerTestAccess::clear(tm);
    ToastManagerTestAccess::inject(tm, ToastSeverity::ERROR, "Jog failed: busy", false);

    CHECK_FALSE(
        ToastManagerTestAccess::refresh_duplicate(tm, ToastSeverity::WARNING, "Jog failed: busy"));
    CHECK_FALSE(ToastManagerTestAccess::refresh_duplicate(tm, ToastSeverity::ERROR, "Other error"));
    ToastManagerTestAccess::clear(tm);
}

TEST_CASE("Toast dedupe: exiting toasts don't match", "[toast][dedupe]") {
    auto& tm = ToastManager::instance();
    ToastManagerTestAccess::clear(tm);
    ToastManagerTestAccess::inject(tm, ToastSeverity::ERROR, "Jog failed: busy", true);

    CHECK_FALSE(
        ToastManagerTestAccess::refresh_duplicate(tm, ToastSeverity::ERROR, "Jog failed: busy"));
    ToastManagerTestAccess::clear(tm);
}
