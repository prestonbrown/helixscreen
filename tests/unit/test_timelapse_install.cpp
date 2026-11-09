// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_button.h"
#include "ui_overlay_timelapse_install.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/timelapse_install_overlay_test_access.h"
#include "app_globals.h"
#include "moonraker_config_manager.h"
#include "printer_state.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
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

TEST_CASE_METHOD(LVGLTestFixture, "a screensaver wake resumes the install wizard where it was",
                 "[timelapse][suspend]") {
    TimelapseInstallOverlay overlay(nullptr);
    overlay.on_activate();
    TimelapseInstallOverlayTestAccess::park_at_restart(overlay);

    overlay.on_deactivate(DeactivateReason::Suspended);
    overlay.on_activate();

    CHECK(TimelapseInstallOverlayTestAccess::at_restart(overlay));
    CHECK(TimelapseInstallOverlayTestAccess::wizard_active(overlay));

    // Leaving the overlay stops the wizard; opening it again starts over.
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    CHECK_FALSE(TimelapseInstallOverlayTestAccess::wizard_active(overlay));
    overlay.on_activate();
    CHECK_FALSE(TimelapseInstallOverlayTestAccess::at_restart(overlay));
}

// ============================================================================
// Layout: the remote route's action is on screen
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "the SSH step's Check Again sits on screen and the header button stays hidden",
                 "[timelapse][layout][ui_integration]") {
    TimelapseInstallOverlay overlay(nullptr);
    overlay.init_subjects();
    overlay.register_callbacks();
    lv_obj_t* root = overlay.create(test_screen());
    REQUIRE(root != nullptr);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);

    TimelapseInstallOverlayTestAccess::show_remote_route(overlay);
    helix::ui::UpdateQueue::instance().drain();
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

    overlay.cleanup();
}

// ============================================================================
// Local install: runs, reopens, prints
// ============================================================================

namespace {
using helix::CommandResult;
using helix::LocalPluginOutcome;
using helix::LocalPluginResult;

constexpr const char* kWaitsForPrint =
    "Installing restarts Moonraker, so it waits until the print is finished or cancelled.";

/// LVGL, the global PrinterState the wizard reads a job from, and a HOME to clone under.
struct LocalInstallFixture : public LVGLTestFixture {
    LocalInstallFixture() {
        const char* home = std::getenv("HOME");
        had_home_ = home != nullptr;
        saved_home_ = home ? home : "";
        setenv("HOME", "/nonexistent-timelapse-home", 1);
        get_printer_state().init_subjects(false);
        set_job_holds(false);
    }
    ~LocalInstallFixture() override {
        set_job_holds(false);
        if (had_home_) {
            setenv("HOME", saved_home_.c_str(), 1);
        } else {
            unsetenv("HOME");
        }
    }
    static void set_job_holds(bool holds) {
        lv_subject_set_int(get_printer_state().print_state().get_job_holds_machine_subject(),
                           holds ? 1 : 0);
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

  private:
    bool had_home_ = false;
    std::string saved_home_;
};

/// A runner whose every command blocks until released, then succeeds.
struct GatedRunner {
    std::atomic<bool> released{false};
    std::atomic<int> calls{0};

    helix::ProcessRunner runner() {
        return [this](const std::vector<std::string>&, int) {
            ++calls;
            while (!released.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            CommandResult ok;
            ok.started = true;
            ok.exit_code = 0;
            return ok;
        };
    }
};

LocalPluginResult installed() {
    LocalPluginResult r;
    r.outcome = LocalPluginOutcome::Installed;
    return r;
}
} // namespace

TEST_CASE_METHOD(LocalInstallFixture,
                 "closing the wizard mid-install leaves no stuck install: a reopened run installs",
                 "[timelapse][local_install_ui]") {
    using Access = TimelapseInstallOverlayTestAccess;
    TimelapseInstallOverlay overlay(nullptr);
    overlay.init_subjects();
    GatedRunner gate;
    Access::set_runner(overlay, gate.runner());

    overlay.on_activate();
    Access::install_locally(overlay);
    REQUIRE(settle_until([&] { return gate.calls.load() == 1; }));

    overlay.on_deactivate(DeactivateReason::NavigateAway);
    overlay.on_activate();
    Access::install_locally(overlay);
    // The first run's worker is still going: this run says so, and keeps a button.
    CHECK(Access::status(overlay) == "An earlier install is still finishing...");
    CHECK(Access::action_visible(overlay));
    CHECK(gate.calls.load() == 1);

    // When it ends, the reopened run starts its own install and finishes it.
    gate.released.store(true);
    REQUIRE(settle_until([&] { return Access::at_configure(overlay); }));
    CHECK(gate.calls.load() > 1);
    CHECK_FALSE(Access::install_running(overlay));
    overlay.cleanup();
}

TEST_CASE_METHOD(LocalInstallFixture, "a closed run's install result never drives a reopened run",
                 "[timelapse][local_install_ui]") {
    using Access = TimelapseInstallOverlayTestAccess;
    TimelapseInstallOverlay overlay(nullptr);
    overlay.init_subjects();

    overlay.on_activate();
    const unsigned first = Access::generation(overlay);
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    overlay.on_activate();
    const unsigned second = Access::generation(overlay);
    REQUIRE(second != first);
    Access::park_at_install(overlay);

    Access::finish_install(overlay, first, installed());
    CHECK(Access::at_install(overlay));

    // The run's own result does move it on.
    Access::finish_install(overlay, second, installed());
    CHECK(Access::at_configure(overlay));
    overlay.cleanup();
}

TEST_CASE_METHOD(LocalInstallFixture,
                 "a print started during the install holds configure and the restart",
                 "[timelapse][local_install_ui]") {
    using Access = TimelapseInstallOverlayTestAccess;
    TimelapseInstallOverlay overlay(nullptr);
    overlay.init_subjects();
    overlay.on_activate();
    Access::park_at_install(overlay);

    set_job_holds(true);
    Access::finish_install(overlay, Access::generation(overlay), installed());
    CHECK(Access::at_configure(overlay));
    CHECK(Access::status(overlay) == kWaitsForPrint);
    REQUIRE(Access::action_visible(overlay));

    // Check Again while it still prints keeps waiting; once it ends, configure runs.
    REQUIRE(Access::run_action(overlay));
    CHECK(Access::status(overlay) == kWaitsForPrint);
    set_job_holds(false);
    REQUIRE(Access::run_action(overlay));
    CHECK(Access::status(overlay) == "Not connected to printer");

    SECTION("the restart waits too") {
        set_job_holds(true);
        Access::restart_moonraker(overlay);
        CHECK(Access::at_restart(overlay));
        CHECK(Access::status(overlay) == kWaitsForPrint);
    }
    SECTION("so does a Retry of the install") {
        GatedRunner gate;
        gate.released.store(true);
        Access::set_runner(overlay, gate.runner());
        set_job_holds(true);
        Access::install_locally(overlay);
        CHECK(Access::status(overlay) == kWaitsForPrint);
        CHECK_FALSE(Access::install_running(overlay));
        CHECK(gate.calls.load() == 0);
    }
    overlay.cleanup();
}

TEST_CASE_METHOD(LocalInstallFixture, "no HOME to clone under takes the SSH route",
                 "[timelapse][local_install_ui]") {
    using Access = TimelapseInstallOverlayTestAccess;
    TimelapseInstallOverlay overlay(nullptr);
    overlay.init_subjects();
    GatedRunner gate;
    gate.released.store(true);
    Access::set_runner(overlay, gate.runner());
    overlay.on_activate();

    SECTION("unset") {
        unsetenv("HOME");
    }
    SECTION("empty") {
        setenv("HOME", "", 1);
    }
    Access::install_locally(overlay);
    CHECK(Access::card(overlay) == 1); // Card::Ssh
    CHECK_FALSE(Access::install_running(overlay));
    CHECK(gate.calls.load() == 0);
    overlay.cleanup();
}
