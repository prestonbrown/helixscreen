// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_delegates_homing.cpp
 * @brief delegates_homing_to_printer(): true only when AFC.cfg's [AFC]
 * auto_home is loaded and set (#1265). False-until-loaded is the safety
 * posture — never skip a needed home, at worst one redundant G28.
 */

#include "../lvgl_test_fixture.h"
#include "afc_config_manager.h"
#include "ams_backend_afc.h"
#include "ams_types.h"
#include "printer_state.h"
#include "test_helpers/afc_test_access.h"

#include <algorithm>
#include <functional>
#include <lvgl.h>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
const char* kCfgWithAutoHome = R"(
[AFC]
auto_home: True
)";

const char* kCfgWithoutAutoHome = R"(
[AFC]
tool_start: direct
)";
} // namespace

namespace helix {

// Same friend-based access shape as test_afc_device_actions_config.cpp's
// AmsBackendAfcConfigHelper (declared friend at include/ams_backend_afc.h:520).
class AfcDelegatesHomingHelper : public AmsBackendAfc {
  public:
    AfcDelegatesHomingHelper() : AmsBackendAfc(nullptr, nullptr) {}

    void load_config(const char* content) {
        AfcTestAccess::afc_config(*this) = std::make_unique<AfcConfigManager>(nullptr);
        AfcTestAccess::afc_config(*this)->load_from_string(content, "AFC/AFC.cfg");
    }
};
} // namespace helix

TEST_CASE_METHOD(LVGLTestFixture, "AFC delegates_homing_to_printer reads [AFC] auto_home",
                 "[afc][homing][1265]") {
    AfcDelegatesHomingHelper afc;

    // Config never loaded (first ~1-2s after connect, or fetch failed):
    // conservatively false — our G28 still goes out.
    CHECK_FALSE(afc.delegates_homing_to_printer());

    afc.load_config(kCfgWithoutAutoHome);
    CHECK_FALSE(afc.delegates_homing_to_printer());

    afc.load_config(kCfgWithAutoHome);
    CHECK(afc.delegates_homing_to_printer());
}

namespace helix {

// Drives the REAL ensure_homed_then() path with a captured-gcode API, the
// same shape AfcReassertHelper uses in test_afc_spool_reassert.cpp.
// toolhead_homed() is overridden false rather than driving the homed_axes
// subject: with api_ null the production toolhead_homed() answers "homed"
// without ever reading the subject, so the override is the only route to
// the unhomed branch from a null-api fixture -- the same seam
// HomingProbeBackend uses in test_ams_home_confirmation.cpp.
class AfcDispatchHelper : public AmsBackendAfc {
  public:
    AfcDispatchHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1"};
        AfcTestAccess::initialize_slots(*this, names);
    }

    void load_config(const char* content) {
        AfcTestAccess::afc_config(*this) = std::make_unique<AfcConfigManager>(nullptr);
        AfcTestAccess::afc_config(*this)->load_from_string(content, "AFC/AFC.cfg");
    }

    AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return AmsErrorHelper::success();
    }

    // Both forms, as in HomingProbeBackend: the test passes on_complete, so
    // dispatch_payload() reaches THIS 2-arg virtual, not the 1-arg one.
    AmsError execute_gcode(const std::string& gcode,
                           helix::SharedCallback<void()> on_complete) override {
        captured.push_back(gcode);
        if (on_complete) {
            on_complete();
        }
        return AmsErrorHelper::success();
    }

    bool toolhead_homed() const override {
        return false;
    }

    std::vector<std::string> captured;
};
} // namespace helix

TEST_CASE_METHOD(LVGLTestFixture,
                 "ensure_homed_then sends no G28 when delegating, and homes first when not (#1265)",
                 "[afc][homing][1265]") {
    AfcDispatchHelper afc;
    afc.load_config(kCfgWithAutoHome);

    bool dispatched = false;
    afc.ensure_homed_then("AFF_LOAD LANE=lane1", [&dispatched]() { dispatched = true; });

    CHECK(dispatched);
    REQUIRE(afc.captured.size() == 1);
    CHECK(afc.captured[0] == "AFF_LOAD LANE=lane1");

    afc.load_config(kCfgWithoutAutoHome);
    afc.captured.clear();

    bool dispatched2 = false;
    afc.ensure_homed_then("AFF_LOAD LANE=lane1", [&dispatched2]() { dispatched2 = true; });

    CHECK(dispatched2);
    REQUIRE(afc.captured.size() == 2);
    CHECK(afc.captured[0] == "G28");
    CHECK(afc.captured[1] == "AFF_LOAD LANE=lane1");
}
