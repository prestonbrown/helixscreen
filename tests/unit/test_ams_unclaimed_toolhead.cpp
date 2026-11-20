// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Filament at the toolhead that no lane claims, with bypass off: HelixScreen
// asks once whether it is the bypass spool or should come out.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_unclaimed_toolhead.h"
#include "core/update_queue.h"
#include "notice.h"
#include "test_helpers/registered_backend.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ams::UnclaimedToolheadPromptGate;
using helix::ams::UnclaimedToolheadReading;

namespace {

UnclaimedToolheadReading unclaimed() {
    UnclaimedToolheadReading r;
    r.settled = true;
    r.filament_unclaimed = true;
    r.bypass_switchable = true;
    return r;
}

} // namespace

TEST_CASE("the unclaimed-toolhead prompt opens once per occurrence", "[ams][unclaimed_toolhead]") {
    UnclaimedToolheadPromptGate gate;

    SECTION("not before the backend settles, nor during a print") {
        UnclaimedToolheadReading r = unclaimed();
        r.settled = false;
        CHECK_FALSE(gate.observe(r));
        r = unclaimed();
        r.printing = true;
        CHECK_FALSE(gate.observe(r));
        CHECK(gate.observe(unclaimed()));
    }

    SECTION("repeat frames ask nothing more, even after the prompt closes") {
        CHECK(gate.observe(unclaimed()));
        CHECK_FALSE(gate.observe(unclaimed()));
        gate.prompt_closed();
        CHECK_FALSE(gate.observe(unclaimed()));
    }

    SECTION("it re-arms once the state clears") {
        CHECK(gate.observe(unclaimed()));
        gate.prompt_closed();
        UnclaimedToolheadReading bypass_on = unclaimed();
        bypass_on.bypass_active = true;
        CHECK_FALSE(gate.observe(bypass_on));
        CHECK(gate.observe(unclaimed()));
    }

    SECTION("a busy backend neither asks nor counts as cleared") {
        UnclaimedToolheadReading loading = unclaimed();
        loading.busy = true;
        CHECK_FALSE(gate.observe(loading));
        CHECK(gate.observe(unclaimed()));
        gate.prompt_closed();
        UnclaimedToolheadReading busy_clear = unclaimed();
        busy_clear.busy = true;
        busy_clear.filament_unclaimed = false;
        CHECK_FALSE(gate.observe(busy_clear));
        CHECK_FALSE(gate.observe(unclaimed()));
    }

    SECTION("a bypass the hardware sets is never asked about") {
        UnclaimedToolheadReading sensor = unclaimed();
        sensor.bypass_switchable = false;
        CHECK_FALSE(gate.observe(sensor));
    }
}

namespace {

class UnclaimedHeadMock : public AmsBackendMock {
  public:
    std::optional<bool> toolhead_filament_unaccounted() const override {
        return unclaimed_;
    }
    AmsAction get_current_action() const override {
        return action_;
    }
    bool unclaimed_ = false;
    AmsAction action_ = AmsAction::IDLE;
};

int g_prompts = 0;
void count_prompt(const UiRequest&) {
    ++g_prompts;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AmsState asks about an unclaimed toolhead once per occurrence",
                 "[ams][unclaimed_toolhead]") {
    g_prompts = 0;
    const UiRequestHandler previous =
        set_ui_request_handler(UiRequestKind::OfferUnclaimedToolheadAsBypass, count_prompt);
    {
        test::RegisteredBackend<UnclaimedHeadMock> reg;
        REQUIRE(reg->get_system_info().supports_bypass);
        REQUIRE_FALSE(reg->is_bypass_active());
        auto& ams = AmsState::instance();

        reg->unclaimed_ = true;
        ams.sync_from_backend();
        CHECK(g_prompts == 0); // the startup grace is still running

        AmsStateTestAccess::end_unclaimed_toolhead_grace(ams);
        ams.sync_from_backend();
        CHECK(g_prompts == 1);
        ams.sync_from_backend();
        ams.unclaimed_toolhead_prompt_closed();
        ams.sync_from_backend();
        CHECK(g_prompts == 1);

        reg->unclaimed_ = false;
        ams.sync_from_backend();
        reg->unclaimed_ = true;
        ams.sync_from_backend();
        CHECK(g_prompts == 2);
        ams.unclaimed_toolhead_prompt_closed();
    }
    set_ui_request_handler(UiRequestKind::OfferUnclaimedToolheadAsBypass, previous);
}

TEST_CASE_METHOD(LVGLTestFixture, "AmsState never asks mid-load", "[ams][unclaimed_toolhead]") {
    g_prompts = 0;
    const UiRequestHandler previous =
        set_ui_request_handler(UiRequestKind::OfferUnclaimedToolheadAsBypass, count_prompt);
    {
        // A load trips the toolhead sensor before the backend names the lane.
        test::RegisteredBackend<UnclaimedHeadMock> reg;
        auto& ams = AmsState::instance();
        AmsStateTestAccess::end_unclaimed_toolhead_grace(ams);
        reg->unclaimed_ = true;
        reg->action_ = AmsAction::LOADING;
        for (int i = 0; i < 3; ++i) {
            ams.sync_from_backend();
        }
        CHECK(g_prompts == 0);
        ams.unclaimed_toolhead_prompt_closed();
    }
    set_ui_request_handler(UiRequestKind::OfferUnclaimedToolheadAsBypass, previous);
}
