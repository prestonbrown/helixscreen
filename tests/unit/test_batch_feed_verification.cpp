// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mock-side prerequisite for batch-feed verification: MoonrakerClientMock
// answers AUTO_FEEDING with the channel_state sequence the feeder firmware
// reports, so AmsBackendSnapmaker's status parse — and the batch cursor that
// will be built on top of it — has real transitions to observe in a unit
// test, through the same handle_status() parse the live WebSocket drives.

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "ams_backend_snapmaker.h"
#include "ams_types.h"
#include "batch_feed_reconcile.h"
#include "core/update_queue.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_env.h"
#include "test_helpers/snapmaker_test_access.h"

#include <algorithm>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// The four feed channels at rest, in the rig's own mix of settled states.
/// The connect-time reconcile ends an interlock only when every channel is.
nlohmann::json settled_feeds() {
    return {{"filament_feed left",
             {{"extruder0", {{"channel_state", "load_finish"}}},
              {"extruder1", {{"channel_state", "preload_finish"}}}}},
            {"filament_feed right",
             {{"extruder2", {{"channel_state", "load_finish"}}},
              {"extruder3", {{"channel_state", "unload_finish"}}}}}};
}

/// A production AmsBackendSnapmaker over the mock API + client, so a batch
/// dispatch runs the real gcode path and the mock client's simulated feeder
/// frames flow back through the subscription the same way live frames do.
struct MockBatchFixture : public LVGLTestFixture {
    MoonrakerClientMock mock_client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<helix::test::RegisteredBackend<helix::AmsBackendSnapmaker>> backend_;

    MockBatchFixture() {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
        backend_ = std::make_unique<helix::test::RegisteredBackend<helix::AmsBackendSnapmaker>>(
            api.get(), &mock_client);
        REQUIRE(backend().start().success());
    }

    helix::AmsBackendSnapmaker& backend() {
        return **backend_;
    }

    /// Run the queued feeder frames until the queue is empty and the backend
    /// is back at idle. Bounded so a wedged walk fails the test instead of
    /// hanging the suite.
    void pump_until_idle() {
        for (int i = 0; i < 100; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            if (backend().get_current_action() == helix::AmsAction::IDLE) {
                return;
            }
        }
        FAIL("pump_until_idle: backend never returned to IDLE (action="
             << helix::ams_action_to_string(backend().get_current_action()) << ")");
    }
};

/// FakeMoonrakerClient with gcode_script recorded, so a reconcile test can
/// assert on what would have gone to the printer. Nothing else needs to do
/// anything: reconcile_on_connect only sends.
struct RecordingFakeClient : helix::test::FakeMoonrakerClient {
    std::vector<std::string> sent_gcode;

    int gcode_script(const std::string& gcode) override {
        sent_gcode.push_back(gcode);
        return 0;
    }

    bool sent_contains(const std::string& fragment) const {
        return std::any_of(sent_gcode.begin(), sent_gcode.end(), [&fragment](const auto& line) {
            return line.find(fragment) != std::string::npos;
        });
    }
};

} // namespace

TEST_CASE_METHOD(MockBatchFixture, "Mock walks each head to its terminal channel state",
                 "[ams][batch][mock]") {
    REQUIRE(backend().load_filament_batch({0, 1}).success());
    pump_until_idle();

    CHECK(backend().channel_snapshot(0).state == "load_finish");
    CHECK(backend().channel_snapshot(1).state == "load_finish");
}

TEST_CASE_METHOD(MockBatchFixture, "Mock can fail a named head", "[ams][batch][mock]") {
    helix::ScopedEnv fail_slot("HELIX_MOCK_BATCH_FAIL_SLOT", "1");
    REQUIRE(backend().load_filament_batch({0, 1}).success());
    helix::ui::UpdateQueue::instance().drain();

    CHECK(backend().channel_snapshot(0).state == "load_finish");
    CHECK(backend().channel_snapshot(1).state == "load_fail");
}

TEST_CASE_METHOD(MockBatchFixture, "Mock walks an unload through its heat step",
                 "[ams][batch][mock]") {
    REQUIRE(backend().unload_filament_batch({2}).success());
    pump_until_idle();

    CHECK(backend().channel_snapshot(2).state == "unload_finish");
    CHECK_FALSE(backend().channel_snapshot(2).filament_detected);
}

TEST_CASE_METHOD(MockBatchFixture, "A batch advances its cursor as heads finish", "[ams][batch]") {
    REQUIRE(backend().load_filament_batch({0, 1}).success());
    pump_until_idle();

    const auto plan = backend().batch_plan();
    CHECK(plan.cursor == 2);
    CHECK_FALSE(plan.active);
    // The final head ends the batch; no progress line outlives it.
    CHECK(backend().get_system_info().operation_detail.empty());
}

TEST_CASE_METHOD(MockBatchFixture, "A second batch is refused while one is running",
                 "[ams][batch]") {
    REQUIRE(backend().load_filament_batch({0, 1}).success());
    // Dispatch is fire-and-forget: the frames are queued, not yet walked, so
    // the plan is mid-batch exactly as it is during the inter-head preheat
    // gap on hardware.
    const auto refused = backend().unload_filament_batch({2});
    CHECK(refused.result == helix::AmsResult::BUSY);

    pump_until_idle();
    REQUIRE_FALSE(backend().batch_plan().active);
    // The finished batch admits the next one — the refusal is a live-plan
    // gate, not a wedge.
    REQUIRE(backend().unload_filament_batch({2}).success());
    pump_until_idle();
}

/// The mock's own feed delivers a whole batch in one drain. A backend subscribed
/// on the harness hears only the frames a case fires, so its plan stays armed.
struct QuietBatchFixture : MockBatchFixture {
    QuietBatchFixture() {
        backend_.reset();
        wired =
            std::make_unique<helix::test::RegisteredWired<helix::AmsBackendSnapmaker>>(api.get());
        helix::SnapmakerTestAccess::set_batch_macro_object(**wired,
                                                           "gcode_macro AUTO_FEEDING_BATCH");
    }
    std::unique_ptr<helix::test::RegisteredWired<helix::AmsBackendSnapmaker>> wired;
};

TEST_CASE_METHOD(QuietBatchFixture, "A doing=false reading retires an active plan",
                 "[ams][batch]") {
    REQUIRE((*wired)->load_filament_batch({0, 1}).success());
    REQUIRE((*wired)->batch_plan().active);

    (*wired)->fire_status(nlohmann::json{{"gcode_macro AUTO_FEEDING_BATCH", {{"doing", false}}}});

    CHECK_FALSE((*wired)->batch_plan().active);
}

TEST_CASE_METHOD(QuietBatchFixture, "doing readings other than false leave the plan alone",
                 "[ams][batch]") {
    for (const auto& doing : {nlohmann::json(true), nlohmann::json(1), nlohmann::json("false"),
                              nlohmann::json(nullptr)}) {
        REQUIRE((*wired)->load_filament_batch({0, 1}).success());
        REQUIRE((*wired)->batch_plan().active);

        (*wired)->fire_status(
            nlohmann::json{{"gcode_macro AUTO_FEEDING_BATCH", {{"doing", doing}}}});
        CHECK((*wired)->batch_plan().active);

        // Retire via the parse's own false reading, so the next iteration's
        // dispatch is not refused by the live-plan gate.
        (*wired)->fire_status(
            nlohmann::json{{"gcode_macro AUTO_FEEDING_BATCH", {{"doing", false}}}});
        CHECK_FALSE((*wired)->batch_plan().active);
    }
}

TEST_CASE_METHOD(MockBatchFixture, "A failed head stops the batch at its cursor", "[ams][batch]") {
    helix::ScopedEnv fail_slot("HELIX_MOCK_BATCH_FAIL_SLOT", "1");
    REQUIRE(backend().load_filament_batch({0, 1}).success());
    // A *_fail state leaves the action at ERROR, which no later frame resolves
    // to IDLE in this walk, so pump_until_idle would spin to its bound. The
    // frames are all queued by dispatch; one drain observes the final state.
    helix::ui::UpdateQueue::instance().drain();

    const auto plan = backend().batch_plan();
    CHECK(plan.cursor == 1); // head 0 finished, head 1 did not
    CHECK_FALSE(plan.active);
    CHECK(backend().get_system_info().operation_detail.find("2") != std::string::npos);
}

TEST_CASE_METHOD(MockBatchFixture, "A failed head ends the firmware batch", "[ams][batch]") {
    helix::SnapmakerTestAccess::set_use_batch_macro(backend(), true);
    helix::ScopedEnv fail_slot("HELIX_MOCK_BATCH_FAIL_SLOT", "1");
    REQUIRE(backend().load_filament_batch({0, 1}).success());
    // A *_fail leaves the action at ERROR, which no later frame resolves to
    // IDLE in this walk, so pump_until_idle would spin to its bound. One
    // drain observes the failure AND the recovery it triggers.
    helix::ui::UpdateQueue::instance().drain();

    const auto plan = backend().batch_plan();
    REQUIRE(plan.cursor == 1);  // head 0 verified; head 1 failed — the case the
    REQUIRE_FALSE(plan.active); // recovery answers
    // Prove the macro shape was actually dispatched, not the bare fallback.
    const auto& history = mock_client.gcode_script_history();
    REQUIRE(std::any_of(history.begin(), history.end(), [](const std::string& line) {
        return line.find("AUTO_FEEDING_BATCH ACTION=START") != std::string::npos;
    }));
    // The chain itself ends with an ACTION=END line and the mock delivers
    // every line even though real Klipper would abort the script at the
    // raise, so the recovery is proven by a SECOND exact-line END beyond the
    // chain's own.
    CHECK(std::count(history.begin(), history.end(), "AUTO_FEEDING_BATCH ACTION=END") >= 2);
}

TEST_CASE_METHOD(MockBatchFixture,
                 "A batch RPC failure clears the interlock of the still-live plan",
                 "[ams][batch]") {
    helix::SnapmakerTestAccess::set_use_batch_macro(backend(), true);
    mock_client.force_next_gcode_error(MoonrakerErrorType::TIMEOUT, "timed out",
                                       "AUTO_FEEDING_BATCH ACTION=START");
    REQUIRE(backend().load_filament_batch({0, 1}).success());
    // The mock simulates every script line before delivering the error, so
    // the queued frames would verify the plan out and the recovery would
    // rightly decline. Re-arm over heads the frames do not name, keeping the
    // dispatch id — the state a genuinely lost response leaves behind: plan
    // active, no terminal in flight.
    const auto dispatched = backend().batch_plan();
    helix::SnapmakerTestAccess::set_batch_plan(backend(), {2, 3}, dispatched.load,
                                               dispatched.direction_label, dispatched.of_label,
                                               dispatched.dispatch_id);
    helix::ui::UpdateQueue::instance().drain();

    // The chain's own trailing END plus the recovery's second one.
    const auto& history = mock_client.gcode_script_history();
    CHECK(std::count(history.begin(), history.end(), "AUTO_FEEDING_BATCH ACTION=END") >= 2);
    // The recovery's END finishes the batch; the plan must not survive it.
    CHECK_FALSE(backend().batch_plan().active);
}

TEST_CASE_METHOD(MockBatchFixture, "A stale batch RPC failure leaves the interlock alone",
                 "[ams][batch]") {
    helix::SnapmakerTestAccess::set_use_batch_macro(backend(), true);
    mock_client.force_next_gcode_error(MoonrakerErrorType::TIMEOUT, "timed out",
                                       "AUTO_FEEDING_BATCH ACTION=START");
    REQUIRE(backend().load_filament_batch({0, 1}).success());
    // Every head verifies before the deferred recovery runs — the late-
    // TIMEOUT shape: a four-head batch finished at minute 4, the RPC times
    // out at minute 10, and END would zero hotends preheated since.
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE_FALSE(backend().batch_plan().active);
    const auto& history = mock_client.gcode_script_history();
    CHECK(std::count(history.begin(), history.end(), "AUTO_FEEDING_BATCH ACTION=END") == 1);
}

TEST_CASE_METHOD(MockBatchFixture, "Without the macro nothing extra is sent", "[ams][batch]") {
    helix::SnapmakerTestAccess::set_use_batch_macro(backend(), false);
    helix::ScopedEnv fail_slot("HELIX_MOCK_BATCH_FAIL_SLOT", "0");
    REQUIRE(backend().load_filament_batch({0}).success());
    // Fail case: single drain, same reason as above.
    helix::ui::UpdateQueue::instance().drain();

    // Firmware without AUTO_FEEDING_BATCH has no interlock to clear, so the
    // recovery must not invent the macro out of thin air.
    const auto& history = mock_client.gcode_script_history();
    CHECK(std::none_of(history.begin(), history.end(), [](const std::string& line) {
        return line.find("AUTO_FEEDING_BATCH") != std::string::npos;
    }));
}

TEST_CASE_METHOD(MockBatchFixture, "The AUTO_FEEDING_BATCH shape advances the cursor too",
                 "[ams][batch]") {
    // Discovery never runs in a unit test, so the capability cache starts
    // false; force the START/DOING/END script shape and prove the cursor still
    // verifies each head through it.
    helix::SnapmakerTestAccess::set_use_batch_macro(backend(), true);
    REQUIRE(backend().load_filament_batch({0, 1}).success());
    // The mock records each script LINE separately; prove the START sentinel
    // of the batch shape actually went out, not a bare-AUTO_FEEDING fallback.
    const auto& history = mock_client.gcode_script_history();
    REQUIRE(std::any_of(history.begin(), history.end(), [](const std::string& line) {
        return line.find("AUTO_FEEDING_BATCH ACTION=START") != std::string::npos;
    }));
    pump_until_idle();

    const auto plan = backend().batch_plan();
    CHECK(plan.cursor == 2);
    CHECK_FALSE(plan.active);
}

TEST_CASE("A stranded batch interlock is cleared at connect", "[ams][batch]") {
    RecordingFakeClient client;
    auto status = nlohmann::json::parse(
        R"({"gcode_macro AUTO_FEEDING_BATCH":{"doing":true,"extruder0_temp":0,
            "extruder1_temp":0,"extruder2_temp":0,"extruder3_temp":0},
            "print_stats":{"state":"standby"},"virtual_sdcard":{"is_active":false}})");
    status.update(settled_feeds());

    helix::batch_feeding::reconcile_on_connect(client, status, "gcode_macro AUTO_FEEDING_BATCH");

    CHECK(client.sent_contains("AUTO_FEEDING_BATCH ACTION=END"));
}

TEST_CASE("A batch another client is running is left alone at connect", "[ams][batch]") {
    // Fluidd or a phone app running AUTO_FEEDING_BATCH leaves print_stats
    // standby and this process holds no plan, so every other guard reads idle.
    RecordingFakeClient client;
    nlohmann::json status = {
        {"gcode_macro AUTO_FEEDING_BATCH", {{"doing", true}}},
        {"print_stats", {{"state", "standby"}}},
        {"virtual_sdcard", {{"is_active", false}}},
    };
    status.update(settled_feeds());
    status["filament_feed right"]["extruder3"]["channel_state"] = "load_feeding";

    helix::batch_feeding::reconcile_on_connect(client, status, "gcode_macro AUTO_FEEDING_BATCH");

    CHECK(client.sent_gcode.empty());
}

TEST_CASE("A snapshot without feed channels cannot vouch for an idle feeder", "[ams][batch]") {
    RecordingFakeClient client;
    const nlohmann::json status = {
        {"gcode_macro AUTO_FEEDING_BATCH", {{"doing", true}}},
        {"print_stats", {{"state", "standby"}}},
        {"virtual_sdcard", {{"is_active", false}}},
    };

    helix::batch_feeding::reconcile_on_connect(client, status, "gcode_macro AUTO_FEEDING_BATCH");

    CHECK(client.sent_gcode.empty());
}

TEST_CASE("A batch interlock during a print is left alone", "[ams][batch]") {
    RecordingFakeClient client;
    const auto status = nlohmann::json::parse(
        R"({"gcode_macro AUTO_FEEDING_BATCH":{"doing":true},
            "print_stats":{"state":"printing"},"virtual_sdcard":{"is_active":true}})");

    helix::batch_feeding::reconcile_on_connect(client, status, "gcode_macro AUTO_FEEDING_BATCH");

    CHECK_FALSE(client.sent_contains("AUTO_FEEDING_BATCH"));
}

TEST_CASE("This session's own live batch is left alone", "[ams][batch]") {
    // A filament batch never leaves print_stats standby, so a mid-batch
    // reconnect lands here with every print guard reading idle — exactly the
    // shape that clears a stranded interlock. Only the caller's "this process
    // dispatched a live batch" vouches for the interlock.
    RecordingFakeClient client;
    const nlohmann::json status = {
        {"gcode_macro AUTO_FEEDING_BATCH", {{"doing", true}}},
        {"print_stats", {{"state", "standby"}}},
        {"virtual_sdcard", {{"is_active", false}}},
    };

    helix::batch_feeding::reconcile_on_connect(client, status, "gcode_macro AUTO_FEEDING_BATCH",
                                               /*local_batch_active=*/true);

    CHECK(client.sent_gcode.empty());
}

TEST_CASE("No doing variable in the payload is a no-op", "[ams][batch]") {
    RecordingFakeClient client;
    const auto status = nlohmann::json::parse(
        R"({"print_stats":{"state":"standby"},"virtual_sdcard":{"is_active":false}})");

    helix::batch_feeding::reconcile_on_connect(client, status, "gcode_macro AUTO_FEEDING_BATCH");

    CHECK(client.sent_gcode.empty());
}

TEST_CASE("A non-bool doing variable is a no-op that does not throw", "[ams][batch]") {
    // `doing` is a save-variable: nothing pins its JSON type across firmware
    // versions, and .value("doing", false) throws type_error.302 on any
    // non-bool, unwinding through the subscribe response callback.
    for (const auto& doing : {nlohmann::json(1), nlohmann::json("1"), nlohmann::json(nullptr)}) {
        RecordingFakeClient client;
        const nlohmann::json status = {
            {"gcode_macro AUTO_FEEDING_BATCH", {{"doing", doing}}},
            {"print_stats", {{"state", "standby"}}},
            {"virtual_sdcard", {{"is_active", false}}},
        };

        REQUIRE_NOTHROW(helix::batch_feeding::reconcile_on_connect(
            client, status, "gcode_macro AUTO_FEEDING_BATCH"));

        CHECK(client.sent_gcode.empty());
    }
}

TEST_CASE("An unreadable print state leaves the interlock alone", "[ams][batch]") {
    // Klipper publishes an object as null until its first get_status has run,
    // and chained .value() on a null throws rather than yielding the default.
    // An unconfirmable print state must read as "cannot confirm" and send
    // nothing: a stranded interlock is recoverable, ending a live batch is not.
    for (const auto& print_stats :
         {nlohmann::json(nullptr), nlohmann::json(7), nlohmann::json{{"state", 3}}}) {
        RecordingFakeClient client;
        const nlohmann::json status = {
            {"gcode_macro AUTO_FEEDING_BATCH", {{"doing", true}}},
            {"print_stats", print_stats},
            {"virtual_sdcard", {{"is_active", false}}},
        };

        REQUIRE_NOTHROW(helix::batch_feeding::reconcile_on_connect(
            client, status, "gcode_macro AUTO_FEEDING_BATCH"));

        CHECK(client.sent_gcode.empty());
    }
}

TEST_CASE("A null virtual_sdcard does not stop an otherwise idle cleanup", "[ams][batch]") {
    // print_stats is the authoritative state; virtual_sdcard is a second veto
    // and is absent on some setups, so an unreadable one must not throw and
    // must not suppress the cleanup a confirmed-idle print state allows.
    RecordingFakeClient client;
    nlohmann::json status = {
        {"gcode_macro AUTO_FEEDING_BATCH", {{"doing", true}}},
        {"print_stats", {{"state", "standby"}}},
        {"virtual_sdcard", nullptr},
    };
    status.update(settled_feeds());

    REQUIRE_NOTHROW(helix::batch_feeding::reconcile_on_connect(client, status,
                                                               "gcode_macro AUTO_FEEDING_BATCH"));

    CHECK(client.sent_contains("AUTO_FEEDING_BATCH ACTION=END"));
}

TEST_CASE("The reconcile reads the macro under the config-case object key", "[ams][batch]") {
    // Klipper preserves the config's case in status object keys, so a printer
    // with [gcode_macro auto_feeding_batch] publishes its state under the
    // lowercase spelling the discovery hands us.
    RecordingFakeClient client;
    auto status = nlohmann::json::parse(
        R"({"gcode_macro auto_feeding_batch":{"doing":true},
            "print_stats":{"state":"standby"},"virtual_sdcard":{"is_active":false}})");
    status.update(settled_feeds());

    helix::batch_feeding::reconcile_on_connect(client, status, "gcode_macro auto_feeding_batch");

    CHECK(client.sent_contains("AUTO_FEEDING_BATCH ACTION=END"));
}
