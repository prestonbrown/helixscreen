// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_upgrade_banner.cpp
 * @brief UpgradeBanner's XML dismiss callback reaches the banner that owns it.
 */

#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "app_globals.h"
#include "config.h"
#include "printer_state.h"
#include "system/update_checker.h"
#include "test_helpers/printer_state_test_access.h"
#include "test_helpers/update_checker_test_access.h"
#include "upgrade_banner.h"
#include "upgrade_nudge.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// An aggressive nudge with a newer release cached and nothing dismissed, so
/// should_show_banner() answers true.
class UpgradeBannerFixture : public XMLTestFixture {
  public:
    UpgradeBannerFixture() {
        PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        UpdateChecker::instance().init();
        UpdateCheckerTestAccess::seed_available_update(UpdateChecker::instance());
        set_nudge("aggressive", "");
        register_component("components/upgrade_banner");
    }

    ~UpgradeBannerFixture() override {
        banner.shutdown();
        set_nudge("off", "");
        UpdateCheckerTestAccess::clear(UpdateChecker::instance());
        helix::ui::UpdateQueue::instance().drain();
        PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
    }

    static void set_nudge(const char* intensity, const char* dismissed) {
        auto* cfg = Config::get_instance();
        cfg->set<std::string>("/upgrade_nudge/intensity", intensity);
        cfg->set<std::string>("/upgrade_nudge/dismissed_version", dismissed);
        UpgradeNudge::instance().reload();
    }

    static lv_obj_t* find(const char* name) {
        return lv_obj_find_by_name(lv_layer_top(), name);
    }

    UpgradeBanner banner;
};

} // namespace

TEST_CASE_METHOD(UpgradeBannerFixture, "upgrade banner: dismiss hides the banner that owns it",
                 "[upgrade-banner][updates]") {
    REQUIRE(UpgradeNudge::instance().should_show_banner());

    banner.init();
    lv_obj_t* root = find("upgrade_banner");
    REQUIRE(root != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

    lv_obj_t* dismiss = find("upgrade_banner_dismiss_btn");
    REQUIRE(dismiss != nullptr);
    lv_obj_send_event(dismiss, LV_EVENT_CLICKED, nullptr);

    CHECK(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));
}
