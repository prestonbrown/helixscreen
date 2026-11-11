// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_button.h"
#include "ui_overlay_timelapse_install.h"
#include "ui_overlay_timelapse_settings.h"
#include "ui_timelapse_macro_card.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/unique_temp_dir.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "macro_manager.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_config_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

// ============================================================================
// has_timelapse_section
// ============================================================================

TEST_CASE("has_timelapse_section finds a live [timelapse] header and nothing else",
          "[timelapse][config]") {
    const struct {
        const char* config;
        bool found;
    } cases[] = {
        {"[printer]\nkinematics: corexy\n\n[timelapse]\n\n[update_manager]\n", true},
        {"[timelapse]\nsome_option: value\n", true}, // first line
        {"[printer]\n[timelapse]", true},            // last line, no newline
        {"[printer]\n  [timelapse]\n", true},        // indented
        {"[printer]\n\t[timelapse]\n", true},        // tab-indented
        {"[printer]\n[timelapse]   \n", true},       // trailing spaces
        {"[printer]\r\n[timelapse]\r\n", true},      // CRLF
        {"", false},
        {"[printer]\nkinematics: corexy\n\n[heater_bed]\n", false},
        {"[printer]\n# [timelapse]\n[heater_bed]\n", false},
        {"[printer]\n  # [timelapse]\n[heater_bed]\n", false},
        {"[printer]\n[timelapse_settings]\n[heater_bed]\n", false},
        {"[printer]\n[update_manager timelapse]\ntype: git_repo\n", false},
    };
    for (const auto& c : cases) {
        INFO(c.config);
        CHECK(TimelapseInstallOverlay::has_timelapse_section(c.config) == c.found);
    }
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
    overlay.init_subjects();
    overlay.register_callbacks();
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

// ============================================================================
// The timelapse macro card both screens share
// ============================================================================

namespace helix::ui {
struct TimelapseMacroCardTestAccess {
    static void set_runner(TimelapseMacroCard& card, ProcessRunner runner) {
        card.runner_override_ = std::move(runner);
    }
    static bool repair_running(const TimelapseMacroCard& card) {
        return card.repair_running_.load();
    }
    /// What the card's destructor does to a running repair.
    static void cancel_repair(TimelapseMacroCard& card) {
        card.repair_cancel_.store(true);
    }
};
} // namespace helix::ui

namespace {
using MacroCard = helix::ui::TimelapseMacroCard::Card;
using CardAccess = helix::ui::TimelapseMacroCardTestAccess;

/// The git a checkout repair runs, as one string per command; answers
/// `git -C <repo> symbolic-ref -q HEAD` with `head_exit` and every other command
/// with success. While `held`, each command waits for release.
struct RepairRunner {
    std::mutex mu;
    std::vector<std::string> calls;
    int head_exit = 0;
    std::atomic<bool> held{false};

    helix::ProcessRunner runner() {
        return [this](const std::vector<std::string>& argv, int) {
            while (held.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::string cmd;
            for (const auto& a : argv) {
                cmd += (cmd.empty() ? "" : " ") + a;
            }
            CommandResult r;
            r.started = true;
            r.exit_code = cmd.find(" symbolic-ref ") != std::string::npos ? head_exit : 0;
            std::lock_guard<std::mutex> lock(mu);
            calls.push_back(cmd);
            return r;
        };
    }
    std::vector<std::string> recorded() {
        std::lock_guard<std::mutex> lock(mu);
        return calls;
    }
};

constexpr const char* kMacroPack = "# helix_macros v2.0.0\n[gcode_macro HELIX_READY]\ngcode:\n";

/// Klipper's objects: the helper macros and/or TIMELAPSE_TAKE_FRAME.
helix::PrinterDiscovery klipper_with(bool helix_macros, bool frame_macro) {
    json objects = json::array({"bed_mesh", "timelapse"});
    if (helix_macros) {
        for (const char* m : {"gcode_macro HELIX_READY", "gcode_macro HELIX_START_PRINT",
                              "gcode_macro HELIX_CLEAN_NOZZLE"}) {
            objects.push_back(m);
        }
    }
    if (frame_macro) {
        objects.push_back("gcode_macro TIMELAPSE_TAKE_FRAME");
    }
    helix::PrinterDiscovery d;
    d.parse_objects(objects);
    return d;
}

/// LocalInstallFixture plus a mock Moonraker whose config root holds helix_macros.cfg.
struct MacroCardFixture : public LocalInstallFixture {
    MacroCardFixture() {
        api_.set_config_files(
            {{"printer.cfg", "[include helix_macros.cfg]\n"}, {"helix_macros.cfg", kMacroPack}});
        helix::ui::timelapse_macro_card().init_subjects();
        // The checkout a local install leaves under HOME.
        repo_ = std::string(std::getenv("HOME")) + "/moonraker-timelapse";
        std::filesystem::create_directories(repo_ + "/klipper_macro");
        std::ofstream(repo_ + "/klipper_macro/timelapse.cfg")
            << "[gcode_macro TIMELAPSE_TAKE_FRAME]\n";
        CardAccess::set_runner(helix::ui::timelapse_macro_card(), git_.runner());
    }
    ~MacroCardFixture() override {
        git_.held.store(false);
        settle_until([] { return !CardAccess::repair_running(helix::ui::timelapse_macro_card()); });
        helix::ui::UpdateQueue::instance().drain();
        CardAccess::set_runner(helix::ui::timelapse_macro_card(), {});
        helix::ui::timelapse_macro_card().clear();
        get_printer_state().set_hardware(helix::PrinterDiscovery{});
        get_printer_state().clear_moonraker_locality();
    }
    std::string repo_;
    RepairRunner git_;
    static MacroCard card() {
        return helix::ui::timelapse_macro_card().card();
    }
    std::string staged_macros() {
        return api_.get_uploaded_config("helix_macros.cfg").value_or("");
    }

    MoonrakerClientMock client_;
    helix::PrinterState state_;
    MoonrakerAPIMock api_{client_, state_};
};
} // namespace

TEST_CASE_METHOD(MacroCardFixture,
                 "the macro card includes the timelapse macros through helix_macros.cfg",
                 "[timelapse][macro_card]") {
    auto& card_ui = helix::ui::timelapse_macro_card();

    SECTION("helper macros loaded and no frame macro: include, then offer the restart") {
        const auto klipper = klipper_with(true, false);
        card_ui.refresh(&api_, klipper, true);
        REQUIRE(settle_until([] { return card() != MacroCard::None; }));
        CHECK(card() == MacroCard::RestartKlipper);
        // The checkout's own file by absolute path, never the config-root symlink.
        CHECK(helix::MacroManager::find_timelapse_include(staged_macros()) ==
              helix::MacroManager::timelapse_include_line(repo_));

        // Never during a job: the tap is refused and the offer stays.
        set_job_holds(true);
        card_ui.restart_klipper();
        CHECK(card() == MacroCard::RestartKlipper);
        card_ui.clear();
    }
    SECTION("no helper macros: the line to add, and helix_macros.cfg untouched") {
        const auto klipper = klipper_with(false, false);
        card_ui.refresh(&api_, klipper, true);
        CHECK(card() == MacroCard::IncludeLine);
        CHECK(staged_macros() == kMacroPack);
    }
    SECTION("the frame macro already loaded: no card, no write") {
        const auto klipper = klipper_with(true, true);
        card_ui.refresh(&api_, klipper, true);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(card() == MacroCard::None);
        CHECK(staged_macros() == kMacroPack);
    }
    SECTION("Klipper's objects not in yet: no card, no write") {
        helix::PrinterDiscovery klipper;
        card_ui.refresh(&api_, klipper, true);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(card() == MacroCard::None);
        CHECK(staged_macros() == kMacroPack);
    }
    SECTION("Moonraker on another host: the checkout's path is unknown, so the line to add") {
        const auto klipper = klipper_with(true, false);
        card_ui.refresh(&api_, klipper, false);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(card() == MacroCard::IncludeLine);
        CHECK(staged_macros() == kMacroPack);
    }
    SECTION("no macro file in the checkout: the line to add, nothing written") {
        std::filesystem::remove_all(repo_);
        const auto klipper = klipper_with(true, false);
        card_ui.refresh(&api_, klipper, true);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(card() == MacroCard::IncludeLine);
        CHECK(staged_macros() == kMacroPack);
    }
    SECTION("helix_macros.cfg cannot be read: the line to add") {
        api_.set_config_files({{"printer.cfg", "[include helix_macros.cfg]\n"}});
        const auto klipper = klipper_with(true, false);
        card_ui.refresh(&api_, klipper, true);
        REQUIRE(settle_until([] { return card() != MacroCard::None; }));
        CHECK(card() == MacroCard::IncludeLine);
    }
}

TEST_CASE_METHOD(MacroCardFixture,
                 "opening timelapse settings on an installed plugin shows the macro card",
                 "[timelapse][macro_card]") {
    get_printer_state().set_hardware(klipper_with(true, false));
    get_printer_state().set_moonraker_is_remote(false);
    TimelapseSettingsOverlay overlay(&api_);
    overlay.init_subjects();
    overlay.on_activate();

    REQUIRE(settle_until([] { return card() != MacroCard::None; }));
    CHECK(card() == MacroCard::RestartKlipper);
    CHECK(helix::MacroManager::has_timelapse_include(staged_macros()));
}

TEST_CASE_METHOD(MacroCardFixture,
                 "timelapse settings says nothing before Klipper's objects arrive",
                 "[timelapse][macro_card]") {
    get_printer_state().set_hardware(helix::PrinterDiscovery{});
    get_printer_state().set_moonraker_is_remote(false);
    TimelapseSettingsOverlay overlay(&api_);
    overlay.init_subjects();
    overlay.on_activate();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(card() == MacroCard::None);
    CHECK(staged_macros() == kMacroPack);
}

TEST_CASE_METHOD(MacroCardFixture,
                 "the macro check puts a detached timelapse checkout back on main",
                 "[timelapse][macro_card][checkout_repair]") {
    auto& card_ui = helix::ui::timelapse_macro_card();
    const std::string git = "git -C " + repo_ + " ";
    const auto repaired = [this] {
        return !CardAccess::repair_running(helix::ui::timelapse_macro_card()) &&
               !git_.recorded().empty();
    };

    SECTION("detached and clean, with the macros already loaded: switched to main") {
        git_.head_exit = 1;
        const auto klipper = klipper_with(true, true);
        card_ui.refresh(&api_, klipper, true);
        REQUIRE(settle_until(repaired));
        CHECK(git_.recorded() == std::vector<std::string>{
                                     git + "symbolic-ref -q HEAD",
                                     git + "status --porcelain",
                                     git + "rev-parse -q --verify refs/heads/main",
                                     git + "merge-base --is-ancestor HEAD refs/heads/main",
                                     git + "checkout -q main",
                                 });
        CHECK(card() == MacroCard::None);
        // Moonraker re-reads the entry, so clients stop reporting it detached.
        helix::ui::UpdateQueue::instance().drain();
        CHECK(client_.last_update_refresh_name() == "timelapse");
    }
    SECTION("already on a branch: one look, nothing changed") {
        git_.head_exit = 0;
        const auto klipper = klipper_with(true, true);
        card_ui.refresh(&api_, klipper, true);
        REQUIRE(settle_until(repaired));
        CHECK(git_.recorded() == std::vector<std::string>{git + "symbolic-ref -q HEAD"});
        helix::ui::UpdateQueue::instance().drain();
        CHECK(client_.last_update_refresh_name().empty());

        card_ui.refresh(&api_, klipper, true);
        REQUIRE(settle_until([this] { return git_.recorded().size() == 2; }));
        CHECK(git_.recorded().back() == git + "symbolic-ref -q HEAD");
    }
    SECTION("a job holds the machine: no git until a later open") {
        git_.head_exit = 1;
        set_job_holds(true);
        const auto klipper = klipper_with(true, true);
        card_ui.refresh(&api_, klipper, true);
        helix::ui::UpdateQueue::instance().drain();
        CHECK_FALSE(CardAccess::repair_running(card_ui));
        CHECK(git_.recorded().empty());

        set_job_holds(false);
        card_ui.refresh(&api_, klipper, true);
        REQUIRE(settle_until(repaired));
        CHECK(git_.recorded().back() == git + "checkout -q main");
    }
    SECTION("cancelled during the probes: the checkout never starts") {
        git_.head_exit = 1;
        git_.held.store(true);
        const auto klipper = klipper_with(true, true);
        card_ui.refresh(&api_, klipper, true);
        CardAccess::cancel_repair(card_ui);
        git_.held.store(false);
        REQUIRE(settle_until(repaired));
        helix::ui::UpdateQueue::instance().drain();
        CHECK(git_.recorded().back() == git + "merge-base --is-ancestor HEAD refs/heads/main");
        CHECK(client_.last_update_refresh_name().empty());
    }
    SECTION("Moonraker on another host: no git at all") {
        git_.head_exit = 1;
        const auto klipper = klipper_with(true, true);
        card_ui.refresh(&api_, klipper, false);
        helix::ui::UpdateQueue::instance().drain();
        CHECK_FALSE(CardAccess::repair_running(card_ui));
        CHECK(git_.recorded().empty());
    }
}

TEST_CASE_METHOD(MacroCardFixture,
                 "a checkout repair started by timelapse settings finishes after it closes",
                 "[timelapse][macro_card][checkout_repair]") {
    get_printer_state().set_hardware(klipper_with(true, true));
    get_printer_state().set_moonraker_is_remote(false);
    git_.head_exit = 1;
    git_.held.store(true);
    {
        TimelapseSettingsOverlay overlay(&api_);
        overlay.init_subjects();
        overlay.on_activate();
        REQUIRE(CardAccess::repair_running(helix::ui::timelapse_macro_card()));
        overlay.on_deactivate(DeactivateReason::NavigateAway);
    }
    git_.held.store(false);
    REQUIRE(settle_until(
        [] { return !CardAccess::repair_running(helix::ui::timelapse_macro_card()); }));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(git_.recorded().back() == "git -C " + repo_ + " checkout -q main");
}
