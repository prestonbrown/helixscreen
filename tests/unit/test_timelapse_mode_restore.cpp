// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_config_write_counter.h"
#include "core/subject.h"
#include "core/update_queue.h"
#include "i_moonraker_sub_apis.h"
#include "printer_pending_restore.h"
#include "timelapse_state.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ReportedPrintActivity;
using helix::TimelapseState;
using helix::timelapse::decode_owed;
using helix::timelapse::encode_owed;
using helix::timelapse::kModeRestoreKey;
using helix::timelapse::OwedSetting;
using Values = std::vector<std::pair<std::string, std::string>>;

namespace {

/// The plugin as a restore sees it: its settings, answered on read, changed by
/// each write when that write lands. With hold_writes, a write waits for land().
class FakeTimelapsePlugin : public helix::ITimelapseAPI {
  public:
    TimelapseSettings settings;
    int reads = 0;
    std::vector<Values> writes;
    bool hold_writes = false;

    FakeTimelapsePlugin() {
        settings.enabled = true;
        settings.mode = "layermacro";
        settings.parkhead = true;
    }

    void get_timelapse_settings(TimelapseSettingsCallback on_success, ErrorCallback) override {
        ++reads;
        on_success(settings);
    }
    void set_timelapse_values(const Values& values, SuccessCallback on_success,
                              ErrorCallback) override {
        writes.push_back(values);
        held_.push_back({values, on_success});
        if (!hold_writes) {
            land();
        }
    }
    /// The oldest held write reaches the plugin and answers.
    void land() {
        if (held_.empty()) {
            return;
        }
        auto [values, done] = held_.front();
        held_.erase(held_.begin());
        for (const auto& [key, value] : values) {
            if (key == "mode") {
                settings.mode = value;
            } else if (key == "parkhead") {
                settings.parkhead = value == "True";
            } else if (key == "enabled") {
                settings.enabled = value == "True";
            }
        }
        done();
    }
    size_t held() const {
        return held_.size();
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

  private:
    std::vector<std::pair<Values, SuccessCallback>> held_;
};

const std::vector<OwedSetting> kHyperlapseOwed = {{"mode", "layermacro", "hyperlapse"},
                                                  {"parkhead", "True", "False"}};

class RestoreFixture : public LVGLTestFixture {
  public:
    RestoreFixture() {
        TimelapseState::instance().forget_mode_restore();
        lifecycle.init_int(static_cast<int>(PrintState::Complete)); // the previous job's end
    }
    ~RestoreFixture() override {
        plugin.hold_writes = false;
        while (plugin.held() > 0) {
            plugin.land();
            drain();
        }
        TimelapseState::instance().forget_mode_restore();
    }
    void drain() {
        // A read, its write and that write's answer each land on a later pass.
        for (int pass = 0; pass < 4; ++pass) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }
    void settle(ReportedPrintActivity activity) {
        TimelapseState::instance().settle_mode_restore(&plugin, activity);
        drain();
    }
    void print_goes(PrintState state) {
        lv_subject_set_int(&lifecycle, static_cast<int>(state));
        drain();
    }
    static std::vector<OwedSetting> owed() {
        return decode_owed(helix::pending_restore::read(kModeRestoreKey));
    }
    static void owe(const std::vector<OwedSetting>& settings) {
        helix::pending_restore::write(kModeRestoreKey, encode_owed(settings));
    }

    ScopedConfigWriteCounter store;
    FakeTimelapsePlugin plugin;
    helix::Subject lifecycle;
    SubjectLifetime lifetime = std::make_shared<bool>(true);
};

} // namespace

TEST_CASE("the owed record: what each switch changes and how it is kept",
          "[timelapse][mode-restore]") {
    TimelapseSettings parked;
    parked.mode = "layermacro";
    parked.parkhead = true;
    CHECK(helix::timelapse::hyperlapse_switch(parked) == kHyperlapseOwed);
    parked.parkhead = false;
    CHECK(helix::timelapse::hyperlapse_switch(parked) ==
          std::vector<OwedSetting>{{"mode", "layermacro", "hyperlapse"}});
    CHECK(helix::timelapse::timelapse_off_switch() ==
          std::vector<OwedSetting>{{"enabled", "True", "False"}});

    CHECK(decode_owed(encode_owed(kHyperlapseOwed)) == kHyperlapseOwed);
    CHECK(decode_owed("not a record").empty());

    // A setting owed already keeps the user's own value.
    const auto merged = helix::timelapse::merge_owed(
        kHyperlapseOwed, {{"mode", "hyperlapse", "hyperlapse"}, {"enabled", "True", "False"}});
    CHECK(merged == std::vector<OwedSetting>{{"mode", "layermacro", "hyperlapse"},
                                             {"parkhead", "True", "False"},
                                             {"enabled", "True", "False"}});

    // Only settings still holding what the switch set go back.
    TimelapseSettings now;
    now.mode = "hyperlapse";
    now.parkhead = true; // the user turned parking back on
    CHECK(helix::timelapse::settings_to_restore(kHyperlapseOwed, now) ==
          Values{{"mode", "layermacro"}});
}

TEST_CASE("mode_restore_step: the rule", "[timelapse][mode-restore]") {
    using helix::timelapse::mode_restore_step;
    using helix::timelapse::ModeRestoreStep;
    CHECK(mode_restore_step(false, false, ReportedPrintActivity::Idle) == ModeRestoreStep::Keep);
    CHECK(mode_restore_step(true, true, ReportedPrintActivity::Idle) == ModeRestoreStep::Keep);
    CHECK(mode_restore_step(true, false, ReportedPrintActivity::Active) == ModeRestoreStep::Keep);
    CHECK(mode_restore_step(true, false, ReportedPrintActivity::Unknown) == ModeRestoreStep::Keep);
    CHECK(mode_restore_step(true, false, ReportedPrintActivity::Idle) ==
          ModeRestoreStep::FetchSettings);
}

TEST_CASE_METHOD(RestoreFixture, "after a restart, the switch-back waits out the print",
                 "[timelapse][mode-restore]") {
    owe(kHyperlapseOwed);
    plugin.settings.mode = "hyperlapse";
    plugin.settings.parkhead = false;

    settle(ReportedPrintActivity::Active);
    settle(ReportedPrintActivity::Unknown);
    CHECK(plugin.reads == 0);
    CHECK(owed() == kHyperlapseOwed);

    settle(ReportedPrintActivity::Idle);
    REQUIRE(plugin.writes == std::vector<Values>{{{"mode", "layermacro"}, {"parkhead", "True"}}});
    CHECK(owed().empty());
    CHECK(TimelapseState::instance().layer_mode());
}

TEST_CASE_METHOD(RestoreFixture, "settings the user changed since are left alone",
                 "[timelapse][mode-restore]") {
    owe(kHyperlapseOwed);
    SECTION("both changed: nothing is written and nothing stays owed") {
        plugin.settings.mode = "layermacro";
        plugin.settings.parkhead = true;
        settle(ReportedPrintActivity::Idle);
        CHECK(plugin.reads == 1);
        CHECK(plugin.writes.empty());
    }
    SECTION("only the mode changed: parking still comes back") {
        plugin.settings.mode = "layermacro";
        plugin.settings.parkhead = false;
        settle(ReportedPrintActivity::Idle);
        CHECK(plugin.writes == std::vector<Values>{{{"parkhead", "True"}}});
    }
    CHECK(owed().empty());
}

TEST_CASE_METHOD(RestoreFixture, "a mode the user picks in HelixScreen cancels the restore",
                 "[timelapse][mode-restore]") {
    owe(kHyperlapseOwed);
    plugin.settings.mode = "hyperlapse";
    TimelapseState::instance().forget_mode_restore();
    settle(ReportedPrintActivity::Idle);
    CHECK(plugin.reads == 0);
    CHECK(plugin.writes.empty());
}

TEST_CASE_METHOD(RestoreFixture, "Use Hyperlapse switches for the print and back at its end",
                 "[timelapse][mode-restore]") {
    TimelapseState::instance().record_print_as_hyperlapse(plugin, &lifecycle, lifetime);
    drain();
    REQUIRE(plugin.writes == std::vector<Values>{{{"mode", "hyperlapse"}, {"parkhead", "False"}}});
    CHECK(owed() == kHyperlapseOwed);

    print_goes(PrintState::Printing);
    // A reconnect mid-print keeps it: this session watches the end itself.
    settle(ReportedPrintActivity::Idle);
    CHECK(plugin.writes.size() == 1);

    print_goes(PrintState::Complete);
    REQUIRE(plugin.writes.size() == 2);
    CHECK(plugin.writes[1] == Values{{"mode", "layermacro"}, {"parkhead", "True"}});
    CHECK(owed().empty());
}

TEST_CASE_METHOD(RestoreFixture, "Off for This Print turns timelapse back on at the print's end",
                 "[timelapse][mode-restore]") {
    TimelapseState::instance().begin_print_without_timelapse(plugin, &lifecycle, lifetime);
    CHECK(owed() == helix::timelapse::timelapse_off_switch());
    plugin.settings.enabled = false; // the start's own enabled=False write

    print_goes(PrintState::Printing);
    print_goes(PrintState::Cancelled);
    CHECK(plugin.writes == std::vector<Values>{{{"enabled", "True"}}});
    CHECK(owed().empty());
}

TEST_CASE_METHOD(RestoreFixture, "a start that fails before printing settles at once",
                 "[timelapse][mode-restore]") {
    TimelapseState::instance().begin_print_without_timelapse(plugin, &lifecycle, lifetime);
    plugin.settings.enabled = false;

    SECTION("the start reports its failure") {
        TimelapseState::instance().start_ended_without_print();
        drain();
    }
    SECTION("no failure report: the next connect finds the printer idle") {
        settle(ReportedPrintActivity::Idle);
    }
    CHECK(plugin.writes == std::vector<Values>{{{"enabled", "True"}}});
    CHECK(owed().empty());
}

TEST_CASE_METHOD(RestoreFixture, "a late switch-back answer leaves the next print's record",
                 "[timelapse][mode-restore]") {
    owe(kHyperlapseOwed);
    plugin.settings.mode = "hyperlapse";
    plugin.settings.parkhead = false;
    plugin.hold_writes = true;
    settle(ReportedPrintActivity::Idle); // the switch-back is in flight

    // The next print starts with timelapse off before it lands.
    TimelapseState::instance().begin_print_without_timelapse(plugin, &lifecycle, lifetime);
    plugin.settings.enabled = false;
    plugin.land();
    drain();
    REQUIRE_FALSE(owed().empty());
    const auto now_owed = owed();
    CHECK(std::count(now_owed.begin(), now_owed.end(),
                     helix::timelapse::timelapse_off_switch().front()) == 1);

    plugin.hold_writes = false;
    print_goes(PrintState::Printing);
    print_goes(PrintState::Cancelled);
    REQUIRE(plugin.writes.size() == 2);
    CHECK(plugin.writes[1] == Values{{"enabled", "True"}});
}

TEST_CASE_METHOD(RestoreFixture, "a switch made while a switch-back is in flight lands after it",
                 "[timelapse][mode-restore]") {
    owe(kHyperlapseOwed);
    plugin.settings.mode = "hyperlapse";
    plugin.settings.parkhead = false;
    plugin.hold_writes = true;
    settle(ReportedPrintActivity::Idle);

    TimelapseState::instance().record_print_as_hyperlapse(plugin, &lifecycle, lifetime);
    drain();
    plugin.land(); // the old switch-back
    drain();
    plugin.land(); // then the new switch, read after it
    drain();

    REQUIRE(plugin.writes.size() == 2);
    CHECK(plugin.writes[0] == Values{{"mode", "layermacro"}, {"parkhead", "True"}});
    CHECK(plugin.writes[1] == Values{{"mode", "hyperlapse"}, {"parkhead", "False"}});
    CHECK(plugin.settings.mode == "hyperlapse");
    CHECK(owed() == kHyperlapseOwed);
}
