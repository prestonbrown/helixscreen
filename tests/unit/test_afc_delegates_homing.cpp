// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_delegates_homing.cpp
 * @brief delegates_homing_to_printer(): true only when AFC.cfg's [AFC]
 * auto_home is loaded and set (#1265). False-until-loaded is the safety
 * posture — never skip a needed home, at worst one redundant prompt.
 */

#include "ui_ams_sidebar.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "afc_config_manager.h"
#include "ams_backend_afc.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "printer_state.h"
#include "test_helpers/scoped_home_confirm_prompter.h"
#include "tool_state.h"

#include <algorithm>
#include <functional>
#include <lvgl.h>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using TA = helix::ui::FilamentPanelTestAccess;

namespace {
const char* CFG_WITH_AUTO_HOME = R"(
[AFC]
auto_home: True
)";

const char* CFG_WITHOUT_AUTO_HOME = R"(
[AFC]
tool_start: direct
)";
} // namespace

// Same friend-based access shape as test_afc_device_actions_config.cpp's
// AmsBackendAfcConfigHelper (declared friend at include/ams_backend_afc.h:520).
class AfcDelegatesHomingHelper : public AmsBackendAfc {
  public:
    AfcDelegatesHomingHelper() : AmsBackendAfc(nullptr, nullptr) {}

    void load_config(const char* content) {
        afc_config_ = std::make_unique<AfcConfigManager>(nullptr);
        afc_config_->load_from_string(content, "AFC/AFC.cfg");
    }
};

TEST_CASE_METHOD(LVGLTestFixture, "AFC delegates_homing_to_printer reads [AFC] auto_home",
                 "[afc][homing][1265]") {
    AfcDelegatesHomingHelper afc;

    // Config never loaded (first ~1-2s after connect, or fetch failed):
    // conservatively false — the prompt still fires.
    CHECK_FALSE(afc.delegates_homing_to_printer());

    afc.load_config(CFG_WITHOUT_AUTO_HOME);
    CHECK_FALSE(afc.delegates_homing_to_printer());

    afc.load_config(CFG_WITH_AUTO_HOME);
    CHECK(afc.delegates_homing_to_printer());
}

TEST_CASE_METHOD(LVGLTestFixture, "base default: no backend delegates homing",
                 "[capabilities][homing][1265]") {
    // Qualified call pins the BASE default, matching the
    // printer_reports_spool_ids pattern in test_ams_firmware_persistence.cpp.
    auto afc = std::make_unique<AmsBackendAfc>(nullptr, nullptr);
    CHECK_FALSE(afc->AmsBackend::delegates_homing_to_printer());
}

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
        initialize_slots(names);
    }

    void load_config(const char* content) {
        afc_config_ = std::make_unique<AfcConfigManager>(nullptr);
        afc_config_->load_from_string(content, "AFC/AFC.cfg");
    }

    AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return AmsErrorHelper::success();
    }

    // Both forms, as in HomingProbeBackend: the test passes on_complete, so
    // dispatch_payload() reaches THIS 2-arg virtual, not the 1-arg one.
    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete) override {
        captured.push_back(gcode);
        if (on_complete) {
            on_complete();
        }
        return AmsErrorHelper::success();
    }

    bool toolhead_homed() const override {
        return false;
    }

    bool prompted = false;
    std::vector<std::string> captured;
};

TEST_CASE_METHOD(LVGLTestFixture,
                 "ensure_homed_then dispatches without G28 or prompt when delegating (#1265)",
                 "[afc][homing][1265]") {
    AfcDispatchHelper afc;
    afc.load_config(CFG_WITH_AUTO_HOME);

    // Armed BEFORE the delegating dispatch, so the flag is live on the
    // short-circuit path. Phase 1 discriminates the delegating behavior
    // itself: payload dispatched, no prompt, no G28 -- and the guard must
    // not consume the flag on the way past. Phase 2 is the discriminator
    // for that consume: the SAME still-armed flag (never re-armed) plus a
    // non-delegating config must suppress the prompt via the
    // pre-confirmation and send G28 unprompted. A consume hoisted above
    // the guard would eat the flag in phase 1, leaving phase 2 unarmed --
    // its prompt would fire and CHECK_FALSE(afc.prompted) would fail.
    afc.arm_home_preconfirmed();

    ScopedHomeConfirmPrompter prompter(
        [&afc](std::function<void()> on_confirm, std::function<void()>) {
            afc.prompted = true;
            on_confirm();
        });

    bool dispatched = false;
    afc.ensure_homed_then("AFF_LOAD LANE=lane1", [&dispatched]() { dispatched = true; });

    CHECK(dispatched);
    CHECK_FALSE(afc.prompted);
    // The payload left, and no G28 was synthesized ahead of it.
    REQUIRE(std::find(afc.captured.begin(), afc.captured.end(), "AFF_LOAD LANE=lane1") !=
            afc.captured.end());
    CHECK(std::none_of(afc.captured.begin(), afc.captured.end(),
                       [](const std::string& g) { return g == "G28"; }));

    // Phase 2: non-delegating config, flag still armed from phase 1 (no
    // re-arm). The pre-confirmation must suppress the prompt and the G28
    // must still fire -- proving the delegating dispatch left the flag
    // armed for a later non-delegating one.
    afc.load_config(CFG_WITHOUT_AUTO_HOME);
    afc.prompted = false;
    afc.captured.clear();

    bool dispatched2 = false;
    afc.ensure_homed_then("AFF_LOAD LANE=lane1", [&dispatched2]() { dispatched2 = true; });

    CHECK(dispatched2);
    CHECK_FALSE(afc.prompted);
    REQUIRE(afc.captured.size() == 2);
    CHECK(afc.captured[0] == "G28");
    CHECK(afc.captured[1] == "AFF_LOAD LANE=lane1");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "sidebar and filament-panel pre-prompt predicate skips when delegating",
                 "[ui][homing][1265]") {
    // The predicate shape both UI sites use after this task. A stub backend
    // makes the capability the only variable.
    class DelegatingStub : public AmsBackendAfc {
      public:
        DelegatingStub() : AmsBackendAfc(nullptr, nullptr) {}
        [[nodiscard]] bool delegates_homing_to_printer() const override {
            return true;
        }
    };
    class NotDelegatingStub : public AmsBackendAfc {
      public:
        NotDelegatingStub() : AmsBackendAfc(nullptr, nullptr) {}
    };

    auto should_prompt = [](AmsBackendAfc& b) { return !b.delegates_homing_to_printer(); };
    // (predicate exercised with unhomed state implied: the UI guards ask
    //  !toolhead_is_homed && !delegates — the delegates term alone is pinned
    //  here; the homed term is pre-existing behavior.)

    DelegatingStub d;
    NotDelegatingStub n;
    CHECK_FALSE(should_prompt(d));
    CHECK(should_prompt(n));
}

namespace {

class PrePromptBackend : public AmsBackendMock {
  public:
    PrePromptBackend() : AmsBackendMock(4) {}

    AmsSystemInfo sys_{};

    [[nodiscard]] AmsSystemInfo get_system_info() const override {
        return sys_;
    }
    [[nodiscard]] PathTopology get_topology() const override {
        return PathTopology::HUB;
    }
    [[nodiscard]] AmsType get_type() const override {
        return sys_.type;
    }
    [[nodiscard]] bool requires_slot_selection_for_load() const override {
        return true;
    }
    [[nodiscard]] bool delegates_homing_to_printer() const override {
        return delegating;
    }
    void arm_home_preconfirmed() override {
        ++armed;
    }

    bool delegating = false;
    int armed = 0;
};

AmsSystemInfo pre_prompt_sys() {
    AmsSystemInfo sys;
    sys.type = AmsType::AFC;
    sys.total_slots = 4;
    sys.current_slot = -1;
    sys.filament_loaded = false;
    return sys;
}

struct PanelPrePromptHarness {
    LVGLUITestFixture& fx;
    PrePromptBackend* stub = nullptr;
    std::unique_ptr<FilamentPanel> panel;
    lv_obj_t* root = nullptr;

    explicit PanelPrePromptHarness(LVGLUITestFixture& f) : fx(f) {
        ToolState::instance().init_subjects(true);
        AmsState::instance().init_subjects(true);
        AmsState::instance().clear_backends();

        ToolTopology topo;
        topo.tool_count = 4;
        topo.active_tool = 0;
        topo.tool_to_slot = {0, 1, 2, 3};
        ToolState::instance().set_ams_topology(topo);

        panel = std::make_unique<FilamentPanel>(fx.state(), fx.api());
        panel->init_subjects();

        root = static_cast<lv_obj_t*>(lv_xml_create(fx.test_screen(), "filament_panel", nullptr));
        REQUIRE(root != nullptr);
        panel->setup(root, fx.test_screen());
        fx.process_lvgl(30);

        TA::populate_extruder_dropdown(*panel);
        lv_obj_t* dd = TA::extruder_dropdown(*panel);
        REQUIRE(dd != nullptr);
    }

    void install_stub(bool delegating) {
        auto owned = std::make_unique<PrePromptBackend>();
        owned->sys_ = pre_prompt_sys();
        owned->delegating = delegating;
        stub = owned.get();
        AmsState::instance().set_backend(std::move(owned));
        AmsState::instance().sync_from_backend();
        fx.process_lvgl(10);
    }

    ~PanelPrePromptHarness() {
        fx.process_lvgl(10);
        if (root) {
            lv_obj_delete(root);
        }
        fx.process_lvgl(10);
        panel.reset();
        AmsState::instance().set_backend(nullptr);
        ToolState::instance().clear_ams_topology();
        AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "sidebar pre-preheat home prompt skips only when the backend delegates (#1265)",
                 "[ui][homing][1265]") {
    PrinterState state;
    state.init_subjects(false);
    AmsState::instance().init_subjects(true);

    auto owned = std::make_unique<PrePromptBackend>();
    owned->sys_ = pre_prompt_sys();
    auto* stub = owned.get();
    AmsState::instance().set_backend(std::move(owned));
    AmsState::instance().sync_from_backend();

    int prompts = 0;
    ScopedHomeConfirmPrompter prompter(
        [&prompts](std::function<void()>, std::function<void()>) { ++prompts; });

    {
        ui::AmsOperationSidebar sidebar(state);

        stub->delegating = true;
        sidebar.handle_load_with_preheat(0);
        CHECK(prompts == 0);
        CHECK(stub->armed == 0);

        stub->delegating = false;
        sidebar.handle_load_with_preheat(1);
        CHECK(prompts == 1);
        CHECK(stub->armed == 0);
    }

    AmsState::instance().set_backend(nullptr);
    AmsState::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "filament panel pre-preheat home prompt fires with no backend (#1265)",
                 "[ui][homing][1265][filament]") {
    PanelPrePromptHarness h(*this);
    REQUIRE(AmsState::instance().get_backend() == nullptr);

    int prompts = 0;
    ScopedHomeConfirmPrompter prompter(
        [&prompts](std::function<void()>, std::function<void()>) { ++prompts; });

    TA::handle_load_button(*h.panel);
    CHECK(prompts == 1);

    h.install_stub(false);
    TA::handle_load_button(*h.panel);
    CHECK(prompts == 2);
    CHECK(h.stub->armed == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "filament panel pre-preheat home prompt skips when the backend delegates (#1265)",
                 "[ui][homing][1265]") {
    PanelPrePromptHarness h(*this);
    h.install_stub(true);

    int prompts = 0;
    ScopedHomeConfirmPrompter prompter(
        [&prompts](std::function<void()>, std::function<void()>) { ++prompts; });

    TA::handle_load_button(*h.panel);
    CHECK(prompts == 0);
    CHECK(h.stub->armed == 0);
}
