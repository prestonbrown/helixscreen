// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_manual_pull_prompt.cpp
 * @brief When the "pull the filament out" prompt speaks, and when it stays quiet.
 *
 * Run with: ./build/bin/helix-tests "[manual_pull]"
 *
 * An unload with no lane to retract into (bypass / external spool, or a printer
 * with no AMS at all) leaves filament parked above the extruder with the rest
 * still threaded up the tube. unload_needs_manual_pull() decides WHICH unloads
 * qualify — covered in test_filament_op_dispatch.cpp. This file covers WHEN,
 * which is the part with a race in it: two paths are live at once and the first
 * to fire has to silence the other.
 *
 * The sensor arm is deliberately dead when the toolhead sensor is absent (-1) or
 * already clear (0) at arm time. Both are asserted here, because arming a live
 * observer in those states would toast the instant it registered — before the
 * retract had moved anything — which is the bug this shape exists to avoid.
 */

#include "ui_manual_pull_prompt.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/filament_sensor_manager_test_access.h"
#include "../test_helpers/post_unload_grace_test_access.h"
#include "../ui_test_utils.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::FilamentSensorManager;

namespace {

/// Substring of the prompt copy. Matching on a fragment rather than the whole
/// string keeps this from failing on a wording tweak while still proving it is
/// THIS notification and not some unrelated toast the fixture produced.
constexpr const char* kPromptFragment = "clear of the toolhead";

class ManualPullFixture : public LVGLTestFixture {
  public:
    ManualPullFixture() {
        FilamentSensorManager::instance().init_subjects();
        // The manager publishes its first reading through the queue. Land it now,
        // or the first drain inside a case overwrites the value the case set.
        helix::ui::UpdateQueue::instance().drain();
        toolhead_ = FilamentSensorManager::instance().get_toolhead_detected_subject();
        REQUIRE(toolhead_ != nullptr);
        // The info hook sees the prompt whether it reaches the toast stack or the
        // pre-init startup queue — same idiom the WiFi warning tests use via
        // set_test_notification_warning_hook().
        helix::ui::set_test_notification_info_hook([this](const std::string& msg) {
            if (msg.find(kPromptFragment) != std::string::npos) {
                ++prompts_;
            }
        });
    }

    ~ManualPullFixture() override {
        helix::ui::set_test_notification_info_hook(nullptr);
        helix::ui::disarm_manual_pull_prompt();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Drive the sensor the way FilamentSensorManager::refresh_role_subjects()
    /// does, then let the deferred observer body run.
    void set_toolhead(int value) {
        lv_subject_set_int(toolhead_, value);
        helix::ui::UpdateQueue::instance().drain();
    }

    int prompts_fired() const {
        return prompts_;
    }

  private:
    lv_subject_t* toolhead_ = nullptr;
    int prompts_ = 0;
};

} // namespace

TEST_CASE_METHOD(ManualPullFixture, "Manual pull: the toolhead sensor going clear prompts once",
                 "[manual_pull][bypass]") {
    set_toolhead(1); // filament at the toolhead, as it is when an unload starts
    helix::ui::arm_manual_pull_prompt();
    CHECK(prompts_fired() == 0); // arming alone must say nothing

    set_toolhead(0); // the retract pulls it above the sensor
    CHECK(prompts_fired() == 1);

    // Disarmed by firing. A later reload/unload cycle on a prompt nobody armed
    // must stay silent — this is what stops one bypass unload from prompting on
    // every subsequent lane unload for the rest of the session.
    set_toolhead(1);
    set_toolhead(0);
    CHECK(prompts_fired() == 1);
}

TEST_CASE_METHOD(ManualPullFixture, "Manual pull: no toolhead sensor falls back to completion",
                 "[manual_pull][bypass]") {
    set_toolhead(-1); // -1 is "this printer has no such sensor", not "clear"
    helix::ui::arm_manual_pull_prompt();
    CHECK(prompts_fired() == 0);

    helix::ui::manual_pull_unload_finished();
    CHECK(prompts_fired() == 1);
}

TEST_CASE_METHOD(ManualPullFixture,
                 "Manual pull: arming while already clear does not fire immediately",
                 "[manual_pull][regression]") {
    // observe<int> invokes its handler once at registration. Hooking the
    // sensor up while it already reads 0 would therefore toast instantly, with
    // the filament still gripped by the extruder gears.
    set_toolhead(0);
    helix::ui::arm_manual_pull_prompt();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(prompts_fired() == 0);

    // And the sensor rising afterwards is a LOAD, not an unload. Still silent.
    set_toolhead(1);
    CHECK(prompts_fired() == 0);

    // Completion is what speaks in this state.
    helix::ui::manual_pull_unload_finished();
    CHECK(prompts_fired() == 1);
}

TEST_CASE_METHOD(ManualPullFixture, "Manual pull: a disarmed prompt fires from neither path",
                 "[manual_pull][regression]") {
    set_toolhead(1);
    helix::ui::arm_manual_pull_prompt();
    helix::ui::disarm_manual_pull_prompt();

    set_toolhead(0);
    CHECK(prompts_fired() == 0);

    // The failure path calls disarm; a completion arriving late behind it must
    // not resurrect the prompt.
    helix::ui::manual_pull_unload_finished();
    CHECK(prompts_fired() == 0);
}

TEST_CASE_METHOD(ManualPullFixture, "Manual pull: the sensor wins and completion adds nothing",
                 "[manual_pull][bypass]") {
    // Both paths are live on a sensor-equipped printer. The sensor edge lands
    // first (filament clears the gears well before the backend calls it done),
    // and the completion that follows must not double the toast.
    set_toolhead(1);
    helix::ui::arm_manual_pull_prompt();
    set_toolhead(0);
    REQUIRE(prompts_fired() == 1);

    helix::ui::manual_pull_unload_finished();
    CHECK(prompts_fired() == 1);
}

TEST_CASE_METHOD(ManualPullFixture, "Manual pull: never fires without being armed",
                 "[manual_pull][regression]") {
    // The whole point of the gate: a lane unload never arms, so no amount of
    // sensor traffic or completion signalling may produce the prompt.
    set_toolhead(1);
    set_toolhead(0);
    helix::ui::manual_pull_unload_finished();
    CHECK(prompts_fired() == 0);
}

TEST_CASE_METHOD(ManualPullFixture,
                 "Manual pull: a real sensor edge prompts once the manager's publish drains",
                 "[manual_pull][bypass]") {
    // The app's path: Klipper status reaches the manager, which publishes the
    // toolhead subject through the queue (sync mode off, as in the app), and the
    // prompt observes that subject.
    constexpr const char* kSensor = "filament_switch_sensor toolhead_sensor";
    auto status = [kSensor](bool detected) {
        return nlohmann::json{{kSensor, {{"filament_detected", detected}, {"enabled", true}}}};
    };
    auto& fsm = FilamentSensorManager::instance();
    helix::PostUnloadGraceTestAccess::reset(fsm);
    helix::discover_filament_sensors(fsm, {kSensor});
    fsm.set_role(kSensor, static_cast<uint8_t>(helix::FilamentSensorRole::TOOLHEAD));
    fsm.set_sync_mode(false);

    // Each drain_until_empty() runs the queue to a fixpoint, as successive app ticks do:
    // the manager's publish sets the subject, and the observer it wakes queues
    // its own body behind it.
    fsm.update_from_status(status(true));
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(lv_subject_get_int(fsm.get_toolhead_detected_subject()) == 1);
    helix::ui::arm_manual_pull_prompt();
    CHECK(prompts_fired() == 0);

    fsm.update_from_status(status(false));
    CHECK(prompts_fired() == 0); // nothing reaches the subject until the queue drains
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(lv_subject_get_int(fsm.get_toolhead_detected_subject()) == 0);
    CHECK(prompts_fired() == 1);

    helix::FilamentSensorManagerTestAccess::reset(fsm);
}
