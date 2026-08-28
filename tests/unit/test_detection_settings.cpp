// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "settings_manager.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Drop both detection keys from the shared Config singleton so init_subjects()
/// falls through to the compiled-in defaults (src/system/settings_manager.cpp:214
/// for /detection/enabled, :221 for the per-printer policy) instead of reading
/// back whatever an earlier test persisted.
void erase_detection_keys() {
    Config* config = Config::get_instance();
    auto& data = ConfigTestAccess::data(*config);

    data.erase("detection"); // global: /detection/enabled

    // Per-printer: <df()>detection/policy_u1. df() always ends in '/'.
    const std::string df = config->df();
    const json::json_pointer printer_ptr(df.substr(0, df.size() - 1));
    if (data.contains(printer_ptr) && data.at(printer_ptr).is_object()) {
        data.at(printer_ptr).erase("detection");
    }
}

/// Rebuild every SettingsManager subject from Config.
///
/// The detection getters read the lv_subject and nothing else
/// (src/system/settings_manager.cpp:758, :773), while the setters write BOTH the
/// subject and Config (:764-767, :780-782). A plain set-then-get is therefore a
/// subject->subject round trip: delete the config->set/save half of either setter
/// and it still passes. Re-initialising forces the value to come back out of
/// Config, which is the half that has to survive a restart.
void reload_subjects_from_config() {
    SettingsManager::instance().deinit_subjects();
    SettingsManager::instance().init_subjects();
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "SettingsManager detection_enabled round-trip",
                 "[detection][settings]") {
    erase_detection_keys();
    reload_subjects_from_config();

    SECTION("default is true") {
        REQUIRE(SettingsManager::instance().get_detection_enabled() == true);
    }

    SECTION("false reaches Config and survives a subject rebuild") {
        SettingsManager::instance().set_detection_enabled(false);
        REQUIRE(SettingsManager::instance().get_detection_enabled() == false);
        // Fallback is the OPPOSITE of the expected value, so a missing key fails.
        REQUIRE(Config::get_instance()->get<bool>("/detection/enabled", true) == false);

        reload_subjects_from_config();
        REQUIRE(SettingsManager::instance().get_detection_enabled() == false);
    }

    SECTION("true reaches Config and survives a subject rebuild") {
        SettingsManager::instance().set_detection_enabled(false);
        SettingsManager::instance().set_detection_enabled(true);
        REQUIRE(SettingsManager::instance().get_detection_enabled() == true);
        REQUIRE(Config::get_instance()->get<bool>("/detection/enabled", false) == true);

        reload_subjects_from_config();
        REQUIRE(SettingsManager::instance().get_detection_enabled() == true);
    }

    SettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "SettingsManager detection_policy_u1 round-trip",
                 "[detection][settings]") {
    erase_detection_keys();
    reload_subjects_from_config();

    const std::string policy_path = Config::get_instance()->df() + "detection/policy_u1";

    SECTION("default is 2 (DeferToSource)") {
        REQUIRE(SettingsManager::instance().get_detection_policy_u1() == 2);
    }

    SECTION("each valid policy reaches Config and survives a subject rebuild") {
        for (int policy : {0, 1, 2}) {
            INFO("policy: " << policy);
            SettingsManager::instance().set_detection_policy_u1(policy);
            REQUIRE(SettingsManager::instance().get_detection_policy_u1() == policy);
            REQUIRE(Config::get_instance()->get<int>(policy_path, -1) == policy);

            reload_subjects_from_config();
            REQUIRE(SettingsManager::instance().get_detection_policy_u1() == policy);
        }
    }

    SECTION("out-of-range values clamp into [0, 2]") {
        // std::clamp at src/system/settings_manager.cpp:777 was never driven:
        // every prior case passed an already-valid policy.
        SettingsManager::instance().set_detection_policy_u1(5);
        REQUIRE(SettingsManager::instance().get_detection_policy_u1() == 2);
        REQUIRE(Config::get_instance()->get<int>(policy_path, -1) == 2);

        SettingsManager::instance().set_detection_policy_u1(-1);
        REQUIRE(SettingsManager::instance().get_detection_policy_u1() == 0);
        REQUIRE(Config::get_instance()->get<int>(policy_path, -1) == 0);

        // The clamped value is what persists, not the raw argument.
        reload_subjects_from_config();
        REQUIRE(SettingsManager::instance().get_detection_policy_u1() == 0);
    }

    SettingsManager::instance().deinit_subjects();
}
