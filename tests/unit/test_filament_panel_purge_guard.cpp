// SPDX-License-Identifier: GPL-3.0-or-later

// A purge preamble (home, then travel to the purge bin) runs under the homing
// budget, which is longer than the panel's operation timeout. The operation
// guard has to outlast the preamble, or the panel times the purge out while the
// toolhead is still on its way to the bin and then purges anyway.

#include "ui_panel_filament.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_harness.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "ams_backend_mock.h"
#include "i_moonraker_api.h"
#include "moonraker_api.h"
#include "printer_discovery.h"
#include "standard_macros.h"

#include <lvgl.h>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using TA = helix::ui::FilamentPanelTestAccess;

namespace {

constexpr const char* kBinPreamble = "TEST_BIN_PREAMBLE";

struct BinBackend : AmsBackendMock {
    BinBackend() : AmsBackendMock(2) {}
    [[nodiscard]] std::string purge_preamble_gcode(bool) const override {
        return kBinPreamble;
    }
};

/// Holds every gcode request unanswered, so a test decides when each one answers.
class HoldGcodeAPI : public MoonrakerAPI {
  public:
    using MoonrakerAPI::MoonrakerAPI;

    struct Request {
        std::string script;
        IMoonrakerAPI::SuccessCallback on_success;
    };

    void execute_gcode(const std::string& gcode, IMoonrakerAPI::SuccessCallback on_success,
                       IMoonrakerAPI::ErrorCallback /*on_error*/, uint32_t /*timeout_ms*/ = 0,
                       bool /*silent*/ = false,
                       IMoonrakerAPI::SuccessCallback /*on_queued*/ = nullptr,
                       bool /*caller_surfaces_errors*/ = true) override {
        requests.push_back({gcode, std::move(on_success)});
    }

    /// Answers the first held request containing @p needle with `ok`.
    void answer(const std::string& needle) {
        for (const auto& r : requests) {
            if (r.script.find(needle) != std::string::npos) {
                // A copy: the answer may send another request, growing the vector.
                auto ok = r.on_success;
                REQUIRE(ok);
                ok();
                helix::ui::UpdateQueue::instance().drain();
                return;
            }
        }
        FAIL("no held request contains " << needle);
    }

    [[nodiscard]] bool sent_containing(const std::string& needle) const {
        for (const auto& r : requests) {
            if (r.script.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    std::vector<Request> requests;
};

using PurgeHarness = helix::test::BasicFilamentPanelHarness<HoldGcodeAPI>;

/// Runs LVGL's clock forward by @p ms in one step, firing whatever came due.
void advance_clock(uint32_t ms) {
    lv_tick_inc(ms);
    lv_timer_handler();
    helix::ui::UpdateQueue::instance().drain();
}

/// Waits out the panel's own operation budget.
void outlast_operation_budget() {
    advance_clock(TA::kOperationTimeoutMs + 1000);
}

/// A PURGE macro whose only parameter the panel fills, so no prompt is raised.
void use_purge_macro(PurgeHarness& h) {
    h.cache_macros({{"PURGE", "{% set t = params.PURGE_TEMP|default(240)|int %}\nM109 S{t}"}});
    h.set_extruder_target(255.0);
}

/// No purge macro anywhere, so the purge is the panel's raw-gcode fallback.
void use_purge_fallback() {
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json{"extruder"});
    StandardMacros::instance().reset();
    StandardMacros::instance().init(hardware);
    REQUIRE(StandardMacros::instance().get(StandardMacroSlot::Purge).is_empty());
}

/// Starts the purge and confirms it is waiting on the bin preamble.
void start_purge(PurgeHarness& h) {
    TA::execute_purge(*h.panel);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(h.api.sent_containing(kBinPreamble));
    REQUIRE(TA::operation_active(*h.panel));
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "A purge macro's guard outlasts a slow bin preamble",
                 "[filament][purge_preamble][purge_guard]") {
    PurgeHarness h(std::make_unique<BinBackend>());
    use_purge_macro(h);
    start_purge(h);

    outlast_operation_budget();
    CHECK(TA::operation_active(*h.panel));

    // The macro that follows answers to the panel's own budget.
    h.api.answer(kBinPreamble);
    REQUIRE(h.api.sent_containing("PURGE PURGE_TEMP=255"));
    REQUIRE(TA::operation_active(*h.panel));
    outlast_operation_budget();
    CHECK_FALSE(TA::operation_active(*h.panel));
}

TEST_CASE_METHOD(LVGLUITestFixture, "The purge fallback's guard outlasts a slow bin preamble",
                 "[filament][purge_preamble][purge_guard]") {
    PurgeHarness h(std::make_unique<BinBackend>());
    use_purge_fallback();
    start_purge(h);

    outlast_operation_budget();
    CHECK(TA::operation_active(*h.panel));

    // The extrusion that follows answers to the panel's own budget.
    h.api.answer(kBinPreamble);
    REQUIRE(h.api.sent_containing("G1 E50"));
    REQUIRE(TA::operation_active(*h.panel));
    outlast_operation_budget();
    CHECK_FALSE(TA::operation_active(*h.panel));
}
