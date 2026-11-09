// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_button.h"
#include "ui_overlay_timelapse_install.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/unique_temp_dir.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_config_manager.h"
#include "printer_state.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

// ============================================================================
// has_timelapse_section
// ============================================================================

TEST_CASE("has_timelapse_section: detects existing section", "[timelapse][config]") {
    std::string config = "[printer]\nkinematics: corexy\n\n[timelapse]\n\n[update_manager]\n";
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section(config) == true);
}

TEST_CASE("has_timelapse_section: returns false when missing", "[timelapse][config]") {
    std::string config = "[printer]\nkinematics: corexy\n\n[heater_bed]\n";
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section(config) == false);
}

TEST_CASE("has_timelapse_section: returns false for empty string", "[timelapse][config]") {
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section("") == false);
}

TEST_CASE("has_timelapse_section: ignores commented-out lines", "[timelapse][config]") {
    std::string config = "[printer]\n# [timelapse]\n[heater_bed]\n";
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section(config) == false);
}

TEST_CASE("has_timelapse_section: ignores indented comment lines", "[timelapse][config]") {
    std::string config = "[printer]\n  # [timelapse]\n[heater_bed]\n";
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section(config) == false);
}

TEST_CASE("has_timelapse_section: handles surrounding sections", "[timelapse][config]") {
    std::string config = "[server]\nhost: 0.0.0.0\nport: 7125\n\n"
                         "[timelapse]\n\n"
                         "[update_manager]\n";
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section(config) == true);
}

TEST_CASE("has_timelapse_section: does not match similar section names", "[timelapse][config]") {
    std::string config = "[printer]\n[timelapse_settings]\n[heater_bed]\n";
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section(config) == false);
}

TEST_CASE("has_timelapse_section: does not match update_manager timelapse", "[timelapse][config]") {
    std::string config = "[printer]\n[update_manager timelapse]\ntype: git_repo\n";
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section(config) == false);
}

TEST_CASE("has_timelapse_section: matches with leading whitespace", "[timelapse][config]") {
    std::string config = "[printer]\n  [timelapse]\n";
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section(config) == true);
}

TEST_CASE("has_timelapse_section: matches with leading tab", "[timelapse][config]") {
    std::string config = "[printer]\n\t[timelapse]\n";
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section(config) == true);
}

TEST_CASE("has_timelapse_section: section at very start of file", "[timelapse][config]") {
    std::string config = "[timelapse]\nsome_option: value\n";
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section(config) == true);
}

TEST_CASE("has_timelapse_section: section at end without trailing newline", "[timelapse][config]") {
    std::string config = "[printer]\n[timelapse]";
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section(config) == true);
}

TEST_CASE("has_timelapse_section: handles trailing spaces", "[timelapse][config]") {
    std::string config = "[printer]\n[timelapse]   \n";
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section(config) == true);
}

TEST_CASE("has_timelapse_section: handles Windows line endings", "[timelapse][config]") {
    std::string config = "[printer]\r\n[timelapse]\r\n";
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section(config) == true);
}

// ============================================================================
// append_timelapse_config
// ============================================================================

TEST_CASE("append_timelapse_config: adds timelapse sections", "[timelapse][config]") {
    std::string config = "[printer]\nkinematics: corexy\n";
    std::string result = TimelapseInstallOverlay::append_timelapse_config(config);

    REQUIRE(result.find("[timelapse]") != std::string::npos);
    REQUIRE(result.find("[update_manager timelapse]") != std::string::npos);
    REQUIRE(result.find("type: git_repo") != std::string::npos);
    REQUIRE(result.find("primary_branch: main") != std::string::npos);
    REQUIRE(result.find("path: ~/moonraker-timelapse") != std::string::npos);
    REQUIRE(result.find("origin: https://github.com/mainsail-crew/moonraker-timelapse.git") !=
            std::string::npos);
    REQUIRE(result.find("managed_services: klipper moonraker") != std::string::npos);
}

TEST_CASE("append_timelapse_config: preserves existing content", "[timelapse][config]") {
    std::string config = "[printer]\nkinematics: corexy\nmax_velocity: 300\n";
    std::string result = TimelapseInstallOverlay::append_timelapse_config(config);

    REQUIRE(result.find("[printer]") != std::string::npos);
    REQUIRE(result.find("kinematics: corexy") != std::string::npos);
    REQUIRE(result.find("max_velocity: 300") != std::string::npos);

    // Original content appears before the appended sections
    auto printer_pos = result.find("[printer]");
    auto timelapse_pos = result.find("[timelapse]");
    REQUIRE(printer_pos < timelapse_pos);
}

TEST_CASE("append_timelapse_config: handles config without trailing newline",
          "[timelapse][config]") {
    std::string config = "[printer]\nkinematics: corexy";
    std::string result = TimelapseInstallOverlay::append_timelapse_config(config);

    // Should still produce valid config with sections
    REQUIRE(result.find("[timelapse]") != std::string::npos);
    REQUIRE(result.find("[update_manager timelapse]") != std::string::npos);

    // The original content should still be there
    REQUIRE(result.find("[printer]") != std::string::npos);
    REQUIRE(result.find("kinematics: corexy") != std::string::npos);
}

TEST_CASE("append_timelapse_config: handles empty config", "[timelapse][config]") {
    std::string result = TimelapseInstallOverlay::append_timelapse_config("");

    REQUIRE(result.find("[timelapse]") != std::string::npos);
    REQUIRE(result.find("[update_manager timelapse]") != std::string::npos);
    REQUIRE(result.find("type: git_repo") != std::string::npos);
}

TEST_CASE("append_timelapse_config: includes HelixScreen comment", "[timelapse][config]") {
    std::string config = "[printer]\n";
    std::string result = TimelapseInstallOverlay::append_timelapse_config(config);

    REQUIRE(result.find("# Timelapse - added by HelixScreen") != std::string::npos);
}

TEST_CASE("append_timelapse_config: result is valid with has_timelapse_section",
          "[timelapse][config]") {
    std::string config = "[printer]\nkinematics: corexy\n";

    // Original should not have timelapse section
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section(config) == false);

    // After appending, it should
    std::string result = TimelapseInstallOverlay::append_timelapse_config(config);
    REQUIRE(TimelapseInstallOverlay::has_timelapse_section(result) == true);
}

TEST_CASE("MoonrakerConfigManager produces valid timelapse config", "[timelapse][config_manager]") {
    std::string content = "";
    auto result = helix::MoonrakerConfigManager::add_section(content, "timelapse", {},
                                                             "Timelapse - added by HelixScreen");
    result = helix::MoonrakerConfigManager::add_section(
        result, "update_manager timelapse",
        {{"type", "git_repo"},
         {"primary_branch", "main"},
         {"path", "~/moonraker-timelapse"},
         {"origin", "https://github.com/mainsail-crew/moonraker-timelapse.git"},
         {"managed_services", "klipper moonraker"}});

    REQUIRE(helix::MoonrakerConfigManager::has_section(result, "timelapse") == true);
    REQUIRE(helix::MoonrakerConfigManager::has_section(result, "update_manager timelapse") == true);
    REQUIRE(result.find("type: git_repo") != std::string::npos);
    REQUIRE(result.find("path: ~/moonraker-timelapse") != std::string::npos);
    REQUIRE(result.find("# Timelapse - added by HelixScreen") != std::string::npos);
}

TEST_CASE("Timelapse config can coexist with spoolman config", "[timelapse][config_manager]") {
    std::string content = "";
    content = helix::MoonrakerConfigManager::add_section(
        content, "spoolman", {{"server", "http://1.2.3.4:7912"}}, "Spoolman");
    content = helix::MoonrakerConfigManager::add_section(content, "timelapse", {}, "Timelapse");
    content = helix::MoonrakerConfigManager::add_section(
        content, "update_manager timelapse",
        {{"type", "git_repo"}, {"path", "~/moonraker-timelapse"}});

    REQUIRE(helix::MoonrakerConfigManager::has_section(content, "spoolman") == true);
    REQUIRE(helix::MoonrakerConfigManager::has_section(content, "timelapse") == true);
    REQUIRE(helix::MoonrakerConfigManager::has_section(content, "update_manager timelapse") ==
            true);

    auto without_spoolman = helix::MoonrakerConfigManager::remove_section(content, "spoolman");
    REQUIRE(helix::MoonrakerConfigManager::has_section(without_spoolman, "spoolman") == false);
    REQUIRE(helix::MoonrakerConfigManager::has_section(without_spoolman, "timelapse") == true);
    REQUIRE(helix::MoonrakerConfigManager::has_section(without_spoolman,
                                                       "update_manager timelapse") == true);
}

// ============================================================================
// Screensaver
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "a screensaver wake resumes the install wizard where it was",
                 "[timelapse][suspend]") {
    REQUIRE_FALSE(get_printer_state().has_webcam());
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, get_printer_state());
    TimelapseInstallOverlay overlay(&api);
    REQUIRE(overlay.create(test_screen()) != nullptr);
    lv_obj_t* ssh = lv_obj_find_by_name(overlay.get_panel(), "ssh_instructions_container");
    REQUIRE(ssh != nullptr);

    // The webcam and plugin replies land behind the screensaver and walk the
    // wizard on to its install step.
    overlay.on_activate();
    overlay.on_deactivate(DeactivateReason::Suspended);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE_FALSE(lv_obj_has_flag(ssh, LV_OBJ_FLAG_HIDDEN));
    overlay.on_activate();
    CHECK_FALSE(lv_obj_has_flag(ssh, LV_OBJ_FLAG_HIDDEN));

    // Leaving the overlay ends the run; opening it again starts over.
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    overlay.on_activate();
    CHECK(lv_obj_has_flag(ssh, LV_OBJ_FLAG_HIDDEN));
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    helix::ui::UpdateQueue::instance().drain_until_empty();
}

// ============================================================================
// Layout: the remote route's action is on screen
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "the SSH step's Check Again sits on screen and the header button stays hidden",
                 "[timelapse][layout][ui_integration]") {
    REQUIRE_FALSE(get_printer_state().has_webcam());
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, get_printer_state());
    TimelapseInstallOverlay overlay(&api);
    overlay.init_subjects();
    overlay.register_callbacks();
    lv_obj_t* root = overlay.create(test_screen());
    REQUIRE(root != nullptr);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);

    // Locality unknown and no plugin: the webcam and plugin replies end on the SSH route.
    overlay.on_activate();
    helix::ui::UpdateQueue::instance().drain_until_empty();
    lv_obj_update_layout(test_screen());

    lv_obj_t* footer = lv_obj_find_by_name(root, "install_footer");
    lv_obj_t* button = lv_obj_find_by_name(root, "install_action_button");
    lv_obj_t* header = lv_obj_find_by_name(root, "overlay_header");
    REQUIRE(footer != nullptr);
    REQUIRE(button != nullptr);
    REQUIRE(header != nullptr);
    lv_obj_t* header_action = lv_obj_find_by_name(header, "action_button");
    REQUIRE(header_action != nullptr);

    CHECK_FALSE(lv_obj_has_flag(footer, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_is_visible(button));
    CHECK(lv_obj_has_flag(header_action, LV_OBJ_FLAG_HIDDEN));

    // The SSH card fills the scroll area; the button must still be inside the screen.
    lv_area_t a;
    lv_obj_get_coords(button, &a);
    INFO("button y1=" << a.y1 << " y2=" << a.y2 << " h=" << lv_obj_get_height(button));
    CHECK(lv_obj_get_height(button) > 0);
    CHECK(a.y1 >= 0);
    CHECK(a.y2 < kTestDisplayHeight);

    lv_obj_t* label = ui_button_get_label(button);
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_label_get_text(label)) == "Check Again");
    lv_obj_t* ssh = lv_obj_find_by_name(root, "ssh_instructions_container");
    lv_obj_t* ffmpeg = lv_obj_find_by_name(root, "ffmpeg_instructions_container");
    REQUIRE(ssh != nullptr);
    REQUIRE(ffmpeg != nullptr);
    CHECK_FALSE(lv_obj_has_flag(ssh, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(ffmpeg, LV_OBJ_FLAG_HIDDEN));

    overlay.on_deactivate(DeactivateReason::NavigateAway);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    overlay.cleanup();
}

// ============================================================================
// Local install: runs, reopens, prints
// ============================================================================

namespace {
using helix::CommandResult;

constexpr const char* kWaitsForPrint =
    "Installing restarts Moonraker, so it waits until the print is finished or cancelled.";

/// LVGL, the global PrinterState the wizard reads a job and Moonraker's host from,
/// and a mock Moonraker whose plugin is absent, so an opened wizard reaches step 3.
struct LocalInstallFixture : public LVGLTestFixture {
    LocalInstallFixture() : client(MoonrakerClientMock::PrinterType::VORON_24) {
        const char* home = std::getenv("HOME");
        had_home_ = home != nullptr;
        saved_home_ = home ? home : "";
        std::filesystem::create_directories(home_dir_);
        setenv("HOME", home_dir_.c_str(), 1);
        get_printer_state().init_subjects(false);
        get_printer_state().set_moonraker_is_remote(false);
        set_job_holds(false);
    }
    ~LocalInstallFixture() override {
        set_job_holds(false);
        get_printer_state().clear_moonraker_locality();
        if (had_home_) {
            setenv("HOME", saved_home_.c_str(), 1);
        } else {
            unsetenv("HOME");
        }
        std::error_code ec;
        std::filesystem::remove_all(home_dir_, ec);
    }
    static void set_job_holds(bool holds) {
        lv_subject_set_int(get_printer_state().print_state().get_job_holds_machine_subject(),
                           holds ? 1 : 0);
    }
    static std::string status() {
        return lv_subject_get_string(lv_xml_get_subject(nullptr, "timelapse_install_status"));
    }
    static int card() {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "timelapse_install_card"));
    }
    static bool action_visible() {
        return lv_subject_get_int(
                   lv_xml_get_subject(nullptr, "timelapse_install_action_visible")) != 0;
    }
    /// Drain the UI queue until @p done holds, for up to five seconds.
    template <typename Pred> static bool settle_until(Pred done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            helix::ui::UpdateQueue::instance().drain();
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    MoonrakerClientMock client;

  private:
    bool had_home_ = false;
    std::string saved_home_;
    std::string home_dir_ = helix::test::unique_temp_dir("timelapse_home");
};

/// A runner whose every command blocks until released, then ends with @p exit_code;
/// a successful clone leaves a checkout at its destination.
struct GatedRunner {
    std::atomic<bool> released{false};
    std::atomic<int> calls{0};
    int exit_code = 0;

    helix::ProcessRunner runner() {
        return [this](const std::vector<std::string>& argv, int) {
            ++calls;
            while (!released.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (exit_code == 0 && std::find(argv.begin(), argv.end(), "clone") != argv.end()) {
                std::filesystem::create_directories(argv.back() + "/component");
                std::ofstream(argv.back() + "/component/timelapse.py") << "\n";
            }
            CommandResult done;
            done.started = true;
            done.exit_code = exit_code;
            return done;
        };
    }
};
} // namespace

TEST_CASE_METHOD(LocalInstallFixture,
                 "closing the wizard mid-install: a reopened run waits for it, then installs anew",
                 "[timelapse][local_install_ui]") {
    MoonrakerAPI api(client, get_printer_state());
    TimelapseInstallOverlay overlay(&api);
    overlay.init_subjects();
    GatedRunner gate;
    overlay.set_process_runner(gate.runner());

    overlay.on_activate();
    REQUIRE(settle_until([&] { return gate.calls.load() == 1; }));

    overlay.on_deactivate(DeactivateReason::NavigateAway);
    overlay.on_activate();
    REQUIRE(settle_until([&] { return status() == "An earlier install is still finishing..."; }));
    CHECK(action_visible());
    CHECK(gate.calls.load() == 1);

    // Only the reopened run's own install counts: the first run's result moves nothing.
    gate.released.store(true);
    REQUIRE(settle_until([&] { return gate.calls.load() > 1; }));
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    overlay.cleanup();
}

TEST_CASE_METHOD(LocalInstallFixture, "a print started during the install holds what comes next",
                 "[timelapse][local_install_ui]") {
    MoonrakerAPI api(client, get_printer_state());
    TimelapseInstallOverlay overlay(&api);
    overlay.init_subjects();
    GatedRunner gate;
    overlay.set_process_runner(gate.runner());
    overlay.on_activate();
    REQUIRE(settle_until([&] { return gate.calls.load() == 1; }));

    SECTION("configure waits, and Check Again keeps waiting while it prints") {
        set_job_holds(true);
        gate.released.store(true);
        REQUIRE(settle_until([&] { return status() == kWaitsForPrint; }));
        REQUIRE(action_visible());
        overlay.press_action();
        CHECK(status() == kWaitsForPrint);
    }
    SECTION("a Retry of a failed install waits too, and runs nothing") {
        gate.exit_code = 1;
        gate.released.store(true);
        REQUIRE(settle_until([&] { return action_visible(); }));
        const int calls = gate.calls.load();
        set_job_holds(true);
        overlay.press_action();
        CHECK(status() == kWaitsForPrint);
        CHECK(gate.calls.load() == calls);
    }
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    overlay.cleanup();
}

TEST_CASE_METHOD(LocalInstallFixture, "no HOME to clone under takes the SSH route",
                 "[timelapse][local_install_ui]") {
    MoonrakerAPI api(client, get_printer_state());
    TimelapseInstallOverlay overlay(&api);
    overlay.init_subjects();
    GatedRunner gate;
    gate.released.store(true);
    overlay.set_process_runner(gate.runner());

    SECTION("unset") {
        unsetenv("HOME");
    }
    SECTION("empty") {
        setenv("HOME", "", 1);
    }
    overlay.on_activate();
    REQUIRE(settle_until([&] { return card() == 1; })); // Card::Ssh
    CHECK(gate.calls.load() == 0);
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    overlay.cleanup();
}
