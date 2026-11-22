// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_config_write_counter.h"
#include "core/update_queue.h"
#include "i_moonraker_sub_apis.h"
#include "printer_pending_restore.h"
#include "timelapse_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ReportedPrintActivity;
using helix::TimelapseState;
using helix::timelapse::kModeRestoreKey;
using helix::timelapse::mode_restore_step;
using helix::timelapse::ModeRestoreStep;

namespace {

/// The plugin as far as a mode restore sees it: answers reads with its mode
/// and records every mode write.
class FakeTimelapsePlugin : public helix::ITimelapseAPI {
  public:
    std::string mode = "hyperlapse";
    int reads = 0;
    std::vector<std::string> mode_writes;

    void get_timelapse_settings(TimelapseSettingsCallback on_success, ErrorCallback) override {
        ++reads;
        TimelapseSettings s;
        s.mode = mode;
        on_success(s);
    }
    void set_timelapse_mode(const std::string& m, SuccessCallback on_success,
                            ErrorCallback) override {
        mode_writes.push_back(m);
        mode = m;
        on_success();
    }
    void set_timelapse_settings(const TimelapseSettings&, SuccessCallback, ErrorCallback) override {
    }
    void set_timelapse_enabled(bool, SuccessCallback, ErrorCallback) override {}
    void render_timelapse(SuccessCallback, ErrorCallback) override {}
    void save_timelapse_frames(SuccessCallback, ErrorCallback) override {}
    void get_last_frame_info(std::function<void(const LastFrameInfo&)>, ErrorCallback) override {}
    void get_webcam_list(WebcamListCallback, ErrorCallback) override {}
    void post_webcam(const nlohmann::json&, WebcamCallback, ErrorCallback) override {}
    void delete_webcam(const WebcamInfo&, SuccessCallback, ErrorCallback) override {}
};

/// A fresh session (the app restarted) with a switch-back still owed.
class RestartedSession : public LVGLTestFixture {
  public:
    RestartedSession() {
        TimelapseState::instance().forget_mode_restore();
        helix::pending_restore::write(kModeRestoreKey, "layermacro");
    }
    ~RestartedSession() override {
        TimelapseState::instance().forget_mode_restore();
    }
    void settle(ReportedPrintActivity activity) {
        TimelapseState::instance().settle_mode_restore(&plugin, activity);
        // The read, the write and its answer each land on a later queue pass.
        for (int pass = 0; pass < 3; ++pass) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }
    static std::string owed() {
        return helix::pending_restore::read(kModeRestoreKey);
    }
    ScopedConfigWriteCounter store;
    FakeTimelapsePlugin plugin;
};

} // namespace

TEST_CASE_METHOD(RestartedSession, "a switch-back owed before a restart waits out the print",
                 "[timelapse][mode-restore]") {
    settle(ReportedPrintActivity::Active);
    CHECK(plugin.reads == 0);
    CHECK(plugin.mode_writes.empty());
    CHECK(owed() == "layermacro");

    settle(ReportedPrintActivity::Unknown);
    CHECK(plugin.reads == 0);
    CHECK(owed() == "layermacro");

    // The print has ended: Layer mode comes back and nothing is owed any more.
    settle(ReportedPrintActivity::Idle);
    REQUIRE(plugin.mode_writes == std::vector<std::string>{"layermacro"});
    CHECK(owed().empty());
    CHECK(TimelapseState::instance().layer_mode());
}

TEST_CASE_METHOD(RestartedSession, "a mode other than Hyperlapse after the print drops the restore",
                 "[timelapse][mode-restore]") {
    // Put back already, or the user picked Layer again in another screen.
    plugin.mode = "layermacro";
    settle(ReportedPrintActivity::Idle);
    CHECK(plugin.reads == 1);
    CHECK(plugin.mode_writes.empty());
    CHECK(owed().empty());
}

TEST_CASE_METHOD(RestartedSession, "a mode the user picks in HelixScreen cancels the restore",
                 "[timelapse][mode-restore]") {
    TimelapseState::instance().forget_mode_restore();
    settle(ReportedPrintActivity::Idle);
    CHECK(plugin.reads == 0);
    CHECK(plugin.mode_writes.empty());
}

TEST_CASE("mode_restore_step: the rule", "[timelapse][mode-restore]") {
    const std::string hyper = "hyperlapse";
    const std::string layer = "layermacro";
    CHECK(mode_restore_step(false, false, ReportedPrintActivity::Idle, nullptr) ==
          ModeRestoreStep::Keep);
    // This session watches the print end itself.
    CHECK(mode_restore_step(true, true, ReportedPrintActivity::Idle, nullptr) ==
          ModeRestoreStep::Keep);
    CHECK(mode_restore_step(true, false, ReportedPrintActivity::Active, nullptr) ==
          ModeRestoreStep::Keep);
    CHECK(mode_restore_step(true, false, ReportedPrintActivity::Unknown, nullptr) ==
          ModeRestoreStep::Keep);
    CHECK(mode_restore_step(true, false, ReportedPrintActivity::Idle, nullptr) ==
          ModeRestoreStep::FetchMode);
    CHECK(mode_restore_step(true, false, ReportedPrintActivity::Idle, &hyper) ==
          ModeRestoreStep::Send);
    CHECK(mode_restore_step(true, false, ReportedPrintActivity::Idle, &layer) ==
          ModeRestoreStep::Clear);
}
