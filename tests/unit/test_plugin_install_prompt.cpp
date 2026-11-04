// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_plugin_install_prompt.cpp
 * @brief The HelixPrint plugin install prompt Print Select shows after discovery
 *
 * The prompt opens when discovery reports the plugin absent (0), and only then:
 * never on -1 (not probed yet) or 1 (installed). An install from it must leave
 * Moonraker able to load the plugin, which takes a Moonraker restart, because
 * install.sh always runs with --no-restart. It never opens while a job holds the
 * machine, and at most once a session: not again after it was answered, and not
 * after the user uninstalled the plugin from Advanced. These cases drive the
 * real panels' observers over a mock Moonraker client, with install.sh replaced
 * by a stub.
 */

#include "ui_icon.h"
#include "ui_modal.h"
#include "ui_panel_advanced.h"
#include "ui_panel_print_select.h"

#include "../test_helpers/advanced_panel_test_access.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/helix_plugin_installer_test_access.h"
#include "../test_helpers/plugin_script_sandbox.h"
#include "../test_helpers/plugin_self_heal_test_access.h"
#include "../test_helpers/print_select_detail_view_test_access.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/scoped_env.h"
#include "../ui_test_utils.h"
#include "config.h"
#include "core/observer_factory.h"
#include "discovery_steps.h"
#include "http_executor.h"
#include "printer_state.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// The prompt's preferences open: beta features on, the wizard done, never
/// declined. The whole config is put back afterwards.
struct PromptConfigScope {
    PromptConfigScope() : cfg(Config::get_instance()), saved(ConfigTestAccess::data(*cfg)) {
        cfg->set<bool>("/beta_features", true);
        cfg->set<bool>("/wizard_completed", true);
        cfg->set<bool>(cfg->df() + "wizard_completed", true);
        cfg->set<bool>("/plugin_install_declined", false);
    }
    ~PromptConfigScope() {
        ConfigTestAccess::data(*cfg) = saved;
    }
    PromptConfigScope(const PromptConfigScope&) = delete;
    PromptConfigScope& operator=(const PromptConfigScope&) = delete;

    Config* cfg;
    json saved;
};

struct PromptFixture : PrintSelectGlobalStateReset, PrintSelectPanelFixture {
    PromptConfigScope prefs;
    test::PluginScriptSandbox scripts{PrintSelectPanelTestAccess::plugin_installer(*panel_)};

    PromptFixture() {
        HelixPluginInstallerTestAccess::reset_prompt_session();
        // The startup check of the plugin would run install.sh --relink on
        // every report; its own cases below turn it back on.
        PluginSelfHealTestAccess::set_ran_this_start(true);
        REQUIRE(prompt() == nullptr);
    }

    ~PromptFixture() override {
        HelixPluginInstallerTestAccess::reset_prompt_session();
        PluginSelfHealTestAccess::set_ran_this_start(true);
    }

    void set_plugin_installed(bool installed) {
        get_printer_state().set_helix_plugin_installed(installed);
        drain();
    }

    /// The open install prompt, or null.
    lv_obj_t* prompt() {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        if (dialog && lv_obj_find_by_name(dialog, "btn_install")) {
            return dialog;
        }
        return nullptr;
    }

    void click(const char* name) {
        lv_obj_t* dialog = prompt();
        REQUIRE(dialog != nullptr);
        lv_obj_t* button = lv_obj_find_by_name(dialog, name);
        REQUIRE(button != nullptr);
        // The modal's handlers run inside send_event; no render pass is needed.
        lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
        drain();
    }

    std::string result_message() {
        lv_obj_t* dialog = prompt();
        REQUIRE(dialog != nullptr);
        lv_obj_t* label = lv_obj_find_by_name(dialog, "result_message");
        REQUIRE(label != nullptr);
        return lv_label_get_text(label);
    }

    int moonraker_restarts() const {
        return mock_client_.call_count("server.restart");
    }

    /// Discovery's own plugin probe, run against the mock client.
    void run_plugin_probe() {
        PrinterDiscovery snapshot;
        const json status = json::object();
        DiscoveryContext ctx{*api_,   mock_client_, api_->hardware(), snapshot, status,
                             nullptr, nullptr,      nullptr,          1,        false,
                             false};
        bool ran = false;
        for (const DiscoveryStep& step : discovery_core_steps()) {
            if (std::string(step.name) == "helix_plugin_check") {
                step.run(ctx);
                ran = true;
            }
        }
        REQUIRE(ran);
        drain();
    }

    int plugin_state() {
        return get_printer_state().plugin_status_state().helix_plugin_state();
    }
};

} // namespace

TEST_CASE_METHOD(PromptFixture, "the install prompt opens only when the plugin is known absent",
                 "[plugin_installer][print_select]") {
    SECTION("installed: no prompt") {
        set_plugin_installed(true);
        CHECK(prompt() == nullptr);
    }
    SECTION("absent: the prompt opens") {
        set_plugin_installed(false);
        CHECK(prompt() != nullptr);
    }
}

TEST_CASE_METHOD(PromptFixture, "the install prompt stays closed once declined",
                 "[plugin_installer][print_select]") {
    prefs.cfg->set<bool>("/plugin_install_declined", true);
    set_plugin_installed(false);
    CHECK(prompt() == nullptr);
}

TEST_CASE_METHOD(PromptFixture, "the install prompt stays closed with beta features off",
                 "[plugin_installer][print_select]") {
    prefs.cfg->set<bool>("/beta_features", false);
    set_plugin_installed(false);
    CHECK(prompt() == nullptr);
}

TEST_CASE_METHOD(PromptFixture, "an install from the prompt restarts Moonraker to load the plugin",
                 "[plugin_installer][print_select]") {
    scripts.write_script("exit 0");
    set_plugin_installed(false);
    REQUIRE(prompt() != nullptr);
    const int restarts_before = moonraker_restarts();

    click("btn_install");

    CHECK(scripts.script_args() == "--no-restart --auto ");
    CHECK(moonraker_restarts() == restarts_before + 1);
    CHECK(result_message().find("restarting") != std::string::npos);
}

TEST_CASE_METHOD(PromptFixture, "a failed install from the prompt restarts nothing",
                 "[plugin_installer][print_select]") {
    scripts.write_script("exit 1");
    set_plugin_installed(false);
    REQUIRE(prompt() != nullptr);
    const int restarts_before = moonraker_restarts();

    click("btn_install");

    CHECK(moonraker_restarts() == restarts_before);
    CHECK(result_message().find("restarting") == std::string::npos);

    // The result icon is a label, whose text the failure replaces.
    lv_obj_t* expected = lv_label_create(test_screen());
    helix::ui::icon::set_source(expected, "alert_circle");
    lv_obj_t* icon = lv_obj_find_by_name(prompt(), "result_icon");
    REQUIRE(icon != nullptr);
    CHECK(std::string(lv_label_get_text(icon)) == lv_label_get_text(expected));
}

TEST_CASE_METHOD(PromptFixture,
                 "the detail view's Set up tap opens the prompt wired like the boot-time one",
                 "[plugin_installer][print_select]") {
    PrintSelectPanelTestAccess::build_detail_view(*panel_);
    auto* view = PrintSelectPanelTestAccess::detail_view(*panel_);
    REQUIRE(view != nullptr);
    scripts.write_script("exit 0");

    SECTION("an install restarts Moonraker to load the plugin") {
        PrintSelectDetailViewTestAccess::request_plugin_setup(*view);
        REQUIRE(prompt() != nullptr);
        const int restarts_before = moonraker_restarts();

        click("btn_install");

        CHECK(scripts.script_args() == "--no-restart --auto ");
        CHECK(moonraker_restarts() == restarts_before + 1);
    }

    SECTION("an install is refused while a job holds the machine") {
        PrintSelectDetailViewTestAccess::request_plugin_setup(*view);
        REQUIRE(prompt() != nullptr);
        test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
        drain();
        REQUIRE(job_holds_machine(get_printer_state().print_state().get_print_lifecycle()));

        click("btn_install");

        CHECK(scripts.script_args().empty());
    }
}

TEST_CASE_METHOD(PromptFixture, "the install prompt stays closed while a job holds the machine",
                 "[plugin_installer][print_select]") {
    test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();
    REQUIRE(job_holds_machine(get_printer_state().print_state().get_print_lifecycle()));

    set_plugin_installed(false);

    CHECK(prompt() == nullptr);
}

TEST_CASE_METHOD(PromptFixture, "an answered install prompt does not reopen this session",
                 "[plugin_installer][print_select]") {
    set_plugin_installed(false);
    REQUIRE(prompt() != nullptr);
    click("btn_cancel");
    REQUIRE(prompt() == nullptr);

    // A reconnect reports the plugin absent again; a printer switch or a
    // Moonraker restart can pass through installed on the way.
    set_plugin_installed(false);
    CHECK(prompt() == nullptr);
    set_plugin_installed(true);
    set_plugin_installed(false);
    CHECK(prompt() == nullptr);
}

TEST_CASE_METHOD(PromptFixture, "uninstalling from Advanced does not open the install prompt",
                 "[plugin_installer][print_select][advanced]") {
    helix::ui::modal_init_subjects();
    AdvancedPanel advanced{get_printer_state(), nullptr};
    helix::ui::AdvancedPanelTestAccess::set_uninstall_runner(
        advanced, [](HelixPluginInstaller::UninstallCallback done) {
            done(UninstallOutcome::SUCCESS, "removed");
        });
    helix::ui::AdvancedPanelTestAccess::set_moonraker_restarter(advanced, []() {});
    set_plugin_installed(true);

    helix::ui::AdvancedPanelTestAccess::tap_uninstall_row(advanced);
    drain();
    lv_obj_t* confirm = ModalStack::instance().top_dialog();
    REQUIRE(confirm != nullptr);
    lv_obj_t* uninstall = lv_obj_find_by_name(confirm, "btn_primary");
    REQUIRE(uninstall != nullptr);
    lv_obj_send_event(uninstall, LV_EVENT_CLICKED, nullptr);
    drain();

    REQUIRE(get_printer_state().plugin_status_state().helix_plugin_state() == 0);
    CHECK(prompt() == nullptr);
}

namespace {

struct NotifyCounter {
    int notifications = 0;
};

} // namespace

TEST_CASE_METHOD(PromptFixture, "a repeated absent report does not notify plugin observers",
                 "[plugin_installer][print_select]") {
    // What a reconnect's discovery does: it reports the same 0 again. The
    // subject notifies only on a change, so observers see one transition.
    NotifyCounter counter;
    auto guard = helix::ui::observe<int>(
        get_printer_state().plugin_status_state().get_helix_plugin_installed_subject(), &counter,
        [](NotifyCounter* c, int) { ++c->notifications; },
        get_printer_state().get_subjects_lifetime());
    drain();
    const int at_attach = counter.notifications;

    set_plugin_installed(false);
    set_plugin_installed(false);
    set_plugin_installed(false);

    CHECK(counter.notifications == at_attach + 1);
}

TEST_CASE_METHOD(PromptFixture, "discovery's probe still reads an absent plugin as absent",
                 "[plugin_installer][print_select]") {
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", nullptr);

    run_plugin_probe();

    CHECK(plugin_state() == kHelixPluginAbsent);
    CHECK(prompt() != nullptr);
}

TEST_CASE_METHOD(PromptFixture, "an install prompt deferred by a print opens when the print ends",
                 "[plugin_installer][print_select]") {
    test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();
    set_plugin_installed(false);
    REQUIRE(prompt() == nullptr);

    test::set_wire_state(get_printer_state(), PrintJobState::COMPLETE);
    drain();
    REQUIRE_FALSE(job_holds_machine(get_printer_state().print_state().get_print_lifecycle()));

    // The print's own completion flow gets the screen first.
    CHECK(prompt() == nullptr);
    REQUIRE(PrintSelectPanelTestAccess::plugin_prompt_waiting_for_screen(*panel_));

    PrintSelectPanelTestAccess::recheck_plugin_prompt(*panel_);
    drain();
    CHECK(prompt() != nullptr);
}

TEST_CASE_METHOD(PromptFixture, "a prompt shown after a deferral still opens only once a session",
                 "[plugin_installer][print_select]") {
    test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();
    set_plugin_installed(false);
    test::set_wire_state(get_printer_state(), PrintJobState::COMPLETE);
    drain();
    PrintSelectPanelTestAccess::recheck_plugin_prompt(*panel_);
    drain();
    REQUIRE(prompt() != nullptr);
    click("btn_cancel");
    REQUIRE(prompt() == nullptr);

    test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();
    test::set_wire_state(get_printer_state(), PrintJobState::COMPLETE);
    drain();
    PrintSelectPanelTestAccess::recheck_plugin_prompt(*panel_);
    drain();

    CHECK(prompt() == nullptr);
}

TEST_CASE_METHOD(PromptFixture, "a deferred prompt stays closed when the plugin turns up meanwhile",
                 "[plugin_installer][print_select]") {
    test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();
    set_plugin_installed(false);
    set_plugin_installed(true);

    test::set_wire_state(get_printer_state(), PrintJobState::COMPLETE);
    drain();

    CHECK(prompt() == nullptr);
}

TEST_CASE_METHOD(PromptFixture, "a job that starts while the prompt is open refuses the install",
                 "[plugin_installer][print_select]") {
    std::vector<std::string> warnings;
    helix::ui::set_test_notification_warning_hook(
        [&warnings](const std::string& message) { warnings.push_back(message); });
    scripts.write_script("exit 0");
    set_plugin_installed(false);
    REQUIRE(prompt() != nullptr);
    const int restarts_before = moonraker_restarts();
    test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();

    click("btn_install");

    CHECK(scripts.script_args().empty());
    CHECK(moonraker_restarts() == restarts_before);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("while printing") != std::string::npos);
}

TEST_CASE_METHOD(PromptFixture, "a deferred prompt waits until the completion modal is closed",
                 "[plugin_installer][print_select]") {
    test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();
    set_plugin_installed(false);
    test::set_wire_state(get_printer_state(), PrintJobState::COMPLETE);
    drain();
    // Stands in for the print-completion modal the lifecycle change puts up.
    lv_obj_t* completion = helix::ui::modal_alert("Print Complete", "benchy.gcode");
    drain();
    REQUIRE(completion != nullptr);

    PrintSelectPanelTestAccess::recheck_plugin_prompt(*panel_);
    drain();
    CHECK(prompt() == nullptr);
    CHECK(ModalStack::instance().top_dialog() == completion);
    CHECK(PrintSelectPanelTestAccess::plugin_prompt_waiting_for_screen(*panel_));

    Modal::hide(completion);
    drain();
    REQUIRE(ModalStack::instance().empty());
    PrintSelectPanelTestAccess::recheck_plugin_prompt(*panel_);
    drain();
    CHECK(prompt() != nullptr);
    CHECK_FALSE(PrintSelectPanelTestAccess::plugin_prompt_waiting_for_screen(*panel_));
}

// ============================================================================
// The startup check of an installed plugin (PluginSelfHeal), as the panel runs it
// ============================================================================

namespace {

/// install.sh stands in for --relink with @p relink_exit (0 current, 3 re-pointed,
/// 4 nothing of ours, other failed) and for --restart-moonraker with the exit
/// set_restart_exit() gives (0 restarted); the bundled helix_print.py says @p bundled.
struct SelfHealFixture : PromptFixture {
    SelfHealFixture() {
        PluginSelfHealTestAccess::set_ran_this_start(false);
        set_restart_exit(0);
    }

    void ship(const char* bundled, int relink_exit) {
        std::ofstream(scripts.root + "/moonraker-plugin/helix_print.py")
            << "import os\n\nPLUGIN_VERSION = \"" << bundled << "\"\n";
        scripts.write_script("printf '%s\\n' \"$*\" >> '" + calls_path() + "'\n" +
                             "[ \"$2\" = --relink ] && exit " + std::to_string(relink_exit) +
                             "\n[ \"$1\" = --restart-moonraker ] && exit $(cat '" + scripts.root +
                             "/restart_exit')\nexit 0");
    }

    void set_restart_exit(int code) {
        std::ofstream(scripts.root + "/restart_exit") << code << "\n";
    }

    std::string calls_path() const {
        return scripts.root + "/calls";
    }

    /// Every install.sh run, one per line, as its arguments.
    std::string calls() const {
        std::ifstream in(calls_path());
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    /// Moonraker restarts the check ran install.sh --restart-moonraker for.
    int restarts() const {
        std::ifstream in(calls_path());
        int count = 0;
        for (std::string line; std::getline(in, line);) {
            count += line == "--restart-moonraker" ? 1 : 0;
        }
        return count;
    }

    /// The check's install.sh runs on the slow lane; wait for it, then for its
    /// outcome on this thread.
    void settle() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        drain();
        // An outcome drained here may start the restart, the next script run.
        while (http::HttpExecutor::slow().inflight() > 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            drain();
        }
        drain();
    }

    /// The plugin reads as absent at a new app start, and the check runs again.
    void next_start_reports_absent() {
        set_plugin_installed(true);
        PluginSelfHealTestAccess::set_ran_this_start(false);
        set_plugin_installed(false);
        settle();
    }

    void forget_script_run() {
        std::filesystem::remove(scripts.root + "/args");
    }
};

} // namespace

TEST_CASE_METHOD(SelfHealFixture,
                 "a plugin link left naming a moved install is re-pointed and loaded",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.0.1", 3);
    const int api_restarts_before = moonraker_restarts();
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", nullptr); // the dangling link loaded nothing

    run_plugin_probe();
    settle();

    // server.restart keeps the module Moonraker imported; only a new process loads the new file.
    CHECK(calls() == "--no-restart --relink\n--restart-moonraker\n");
    CHECK(moonraker_restarts() == api_restarts_before);
    // The user installed it: an install prompt has nothing to offer.
    PrintSelectPanelTestAccess::recheck_plugin_prompt(*panel_);
    CHECK(prompt() == nullptr);
}

TEST_CASE_METHOD(
    SelfHealFixture,
    "a plugin set to enabled: False reads as not installed, and an install turns it on",
    "[plugin_installer][print_select][self_heal]") {
    // It answers, but serves nothing, while its link and section stay: the
    // startup check must not take that link as a reason to hold the prompt.
    ship("1.0.1", 0);
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", "disabled");

    run_plugin_probe();
    settle();

    CHECK(plugin_state() == kHelixPluginAbsent);
    CHECK_FALSE(get_printer_state().plugin_status_state().service_has_helix_plugin());
    CHECK(scripts.script_args().empty());
    PrintSelectPanelTestAccess::recheck_plugin_prompt(*panel_);
    REQUIRE(prompt() != nullptr);

    scripts.write_script("exit 0");
    const int restarts_before = moonraker_restarts();
    click("btn_install");
    CHECK(scripts.script_args() == "--no-restart --auto ");
    CHECK(moonraker_restarts() == restarts_before + 1);
    click("btn_ok");
    REQUIRE(prompt() == nullptr);

    ScopedEnv enabled("HELIX_MOCK_HELIX_PLUGIN", "1");
    run_plugin_probe();

    CHECK(plugin_state() == kHelixPluginInstalled);
    CHECK_FALSE(get_printer_state().plugin_status_state().disabled());
    CHECK(prompt() == nullptr);
}

TEST_CASE_METHOD(SelfHealFixture,
                 "a running plugin of another version than the app ships is reloaded",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.2.0", 0);
    const int api_restarts_before = moonraker_restarts();
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", "1"); // the mock reports 1.0.1

    run_plugin_probe();
    settle();

    CHECK(calls() == "--no-restart --relink\n--restart-moonraker\n");
    CHECK(moonraker_restarts() == api_restarts_before);
}

TEST_CASE_METHOD(SelfHealFixture, "a current plugin of the version the app ships is left alone",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.0.1", 0);
    const int restarts_before = restarts();
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", "1");

    run_plugin_probe();
    settle();

    CHECK(scripts.script_args() == "--no-restart --relink ");
    CHECK(restarts() == restarts_before);
}

TEST_CASE_METHOD(SelfHealFixture, "a plugin the user never installed is not installed by the check",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.2.0", 4);
    const int restarts_before = restarts();
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", nullptr);

    run_plugin_probe();
    settle();

    CHECK(scripts.script_args() == "--no-restart --relink ");
    CHECK(restarts() == restarts_before);
    // Absent for real, so the install prompt follows the check.
    PrintSelectPanelTestAccess::recheck_plugin_prompt(*panel_);
    CHECK(prompt() != nullptr);
}

TEST_CASE_METHOD(SelfHealFixture, "a plugin restart waits for the job to let go",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.0.1", 3);
    const int restarts_before = restarts();
    test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();
    REQUIRE(job_holds_machine(get_printer_state().print_state().get_print_lifecycle()));
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", nullptr);

    run_plugin_probe();
    settle();
    CHECK(scripts.script_args() == "--no-restart --relink ");
    CHECK(restarts() == restarts_before);

    test::set_wire_state(get_printer_state(), PrintJobState::COMPLETE);
    drain();
    // The print's own completion flow gets the screen first.
    CHECK(restarts() == restarts_before);
    auto& heal = PrintSelectPanelTestAccess::plugin_self_heal(*panel_);
    REQUIRE(PluginSelfHealTestAccess::restart_waiting_for_screen(heal));

    PluginSelfHealTestAccess::recheck_restart(heal);
    settle();
    CHECK(restarts() == restarts_before + 1);

    test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
    drain();
    PluginSelfHealTestAccess::recheck_restart(heal);
    CHECK(restarts() == restarts_before + 1);
}

TEST_CASE_METHOD(SelfHealFixture,
                 "a deferred plugin restart waits until the completion modal is closed",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.0.1", 3);
    const int restarts_before = restarts();
    test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", nullptr);
    run_plugin_probe();
    settle();
    test::set_wire_state(get_printer_state(), PrintJobState::COMPLETE);
    drain();
    // Stands in for the print-completion modal the lifecycle change puts up.
    lv_obj_t* completion = helix::ui::modal_alert("Print Complete", "benchy.gcode");
    drain();
    REQUIRE(completion != nullptr);
    auto& heal = PrintSelectPanelTestAccess::plugin_self_heal(*panel_);

    PluginSelfHealTestAccess::recheck_restart(heal);
    drain();
    CHECK(restarts() == restarts_before);
    CHECK(PluginSelfHealTestAccess::restart_waiting_for_screen(heal));

    Modal::hide(completion);
    drain();
    REQUIRE(ModalStack::instance().empty());
    PluginSelfHealTestAccess::recheck_restart(heal);
    settle();
    CHECK(restarts() == restarts_before + 1);
    CHECK_FALSE(PluginSelfHealTestAccess::restart_waiting_for_screen(heal));
}

TEST_CASE_METHOD(SelfHealFixture,
                 "a restart Moonraker holds off for a print it reports waits for that job to end",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.0.1", 3);
    set_restart_exit(4);
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", nullptr);
    std::vector<ToastSeverity> toasts;
    helix::ui::set_test_toast_hook(
        [&](ToastSeverity severity, const std::string&, uint32_t) { toasts.push_back(severity); });

    run_plugin_probe();
    settle();
    REQUIRE(restarts() == 1);
    auto& heal = PrintSelectPanelTestAccess::plugin_self_heal(*panel_);
    CHECK(PluginSelfHealTestAccess::restart_waiting_for_job(heal));
    CHECK(toasts.empty());

    set_restart_exit(0);
    test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();
    CHECK(restarts() == 1);
    test::set_wire_state(get_printer_state(), PrintJobState::COMPLETE);
    drain();
    REQUIRE(PluginSelfHealTestAccess::restart_waiting_for_screen(heal));
    PluginSelfHealTestAccess::recheck_restart(heal);
    settle();
    CHECK(restarts() == 2);
    CHECK(toasts == std::vector<ToastSeverity>{ToastSeverity::INFO});
}

TEST_CASE_METHOD(SelfHealFixture,
                 "each --restart-moonraker outcome but a print gets one warning and no wait",
                 "[plugin_installer][print_select][self_heal]") {
    // 3: refused or old code; 6: print state unknown; 7: Moonraker not answering.
    const int code = GENERATE(3, 6, 7);
    CAPTURE(code);
    ship("1.0.1", 3);
    set_restart_exit(code);
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", nullptr);
    std::vector<ToastSeverity> toasts;
    helix::ui::set_test_toast_hook(
        [&](ToastSeverity severity, const std::string&, uint32_t) { toasts.push_back(severity); });

    run_plugin_probe();
    settle();

    CHECK(restarts() == 1);
    CHECK(toasts == std::vector<ToastSeverity>{ToastSeverity::WARNING});
    auto& heal = PrintSelectPanelTestAccess::plugin_self_heal(*panel_);
    CHECK_FALSE(PluginSelfHealTestAccess::restart_waiting_for_job(heal));
    test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();
    test::set_wire_state(get_printer_state(), PrintJobState::COMPLETE);
    drain();
    CHECK(restarts() == 1);
}

TEST_CASE_METHOD(SelfHealFixture,
                 "a finished restart's toast says the plugin was updated and not that Moonraker "
                 "is restarting",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.0.1", 3);
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", nullptr);
    std::vector<std::string> messages;
    helix::ui::set_test_toast_hook(
        [&](ToastSeverity, const std::string& message, uint32_t) { messages.push_back(message); });

    run_plugin_probe();
    settle();

    REQUIRE(restarts() == 1);
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].find("updated") != std::string::npos);
    CHECK(messages[0].find("restarting") == std::string::npos);
}

TEST_CASE_METHOD(SelfHealFixture,
                 "a restart that did not load a plugin Moonraker never ran is tried again",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.0.1", 3);
    set_restart_exit(3);
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", nullptr);

    run_plugin_probe();
    settle();
    REQUIRE(restarts() == 1);

    next_start_reports_absent();
    CHECK(restarts() == 2);
}

TEST_CASE_METHOD(SelfHealFixture,
                 "the restart leaves the print state to install.sh, which asks Moonraker",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.0.1", 3);
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", nullptr);
    ScopedEnv inherited("HELIX_JOB_STATE", "idle");
    scripts.write_script("printf '%s %s\\n' \"$*\" \"${HELIX_JOB_STATE:-unset}\" >> '" +
                         calls_path() + "'\n[ \"$2\" = --relink ] && exit 3\nexit 0");

    run_plugin_probe();
    settle();

    CHECK(calls() == "--no-restart --relink unset\n--restart-moonraker unset\n");
}

TEST_CASE_METHOD(SelfHealFixture,
                 "a restart that did not finish gets its one attempt at the next start",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.2.0", 0);
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", "1"); // the mock reports 1.0.1
    auto next_start = [this]() {
        PluginSelfHealTestAccess::set_ran_this_start(false);
        set_plugin_installed(false);
        run_plugin_probe();
        settle();
    };

    set_restart_exit(1); // nothing restarted Moonraker
    run_plugin_probe();
    settle();
    REQUIRE(restarts() == 1);

    set_restart_exit(4); // held off for a print
    next_start();
    REQUIRE(restarts() == 2);

    set_restart_exit(0);
    next_start();
    REQUIRE(restarts() == 3);

    // That one finished and the versions still differ: no more attempts.
    next_start();
    CHECK(restarts() == 3);
}

TEST_CASE_METHOD(SelfHealFixture,
                 "a restart that needs a manual one is asked once per version pair",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.2.0", 0);
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", "1"); // the mock reports 1.0.1
    std::vector<ToastSeverity> toasts;
    helix::ui::set_test_toast_hook(
        [&](ToastSeverity severity, const std::string&, uint32_t) { toasts.push_back(severity); });
    auto next_start = [this]() {
        PluginSelfHealTestAccess::set_ran_this_start(false);
        set_plugin_installed(false);
        run_plugin_probe();
        settle();
    };

    // 6 and 7 can clear by themselves: the next start tries again.
    set_restart_exit(7);
    run_plugin_probe();
    settle();
    REQUIRE(restarts() == 1);
    set_restart_exit(6);
    next_start();
    REQUIRE(restarts() == 2);

    // Moonraker refused, or kept the old plugin: one warning for this pair.
    set_restart_exit(3);
    next_start();
    REQUIRE(restarts() == 3);
    next_start();
    CHECK(restarts() == 3);
    CHECK(toasts.size() == 3);

    // An app update brings a new pair, which gets one fresh attempt.
    ship("1.3.0", 0);
    set_restart_exit(3);
    next_start();
    CHECK(restarts() == 4);
}

TEST_CASE_METHOD(SelfHealFixture,
                 "a restart still waiting for a job when the app stops is tried at the next start",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.0.1", 3);
    test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", nullptr);

    run_plugin_probe();
    settle();
    REQUIRE(restarts() == 0);

    // The app stops before the job ends; the next start finds no job.
    test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
    drain();
    next_start_reports_absent();
    CHECK(restarts() == 1);
}

TEST_CASE_METHOD(SelfHealFixture,
                 "a version pair a restart did not fix is not restarted for at the next start",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.2.0", 0);
    const int restarts_before = restarts();
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", "1"); // the mock reports 1.0.1

    run_plugin_probe();
    settle();
    REQUIRE(restarts() == restarts_before + 1);

    // The next app start: Moonraker still runs 1.0.1.
    PluginSelfHealTestAccess::set_ran_this_start(false);
    forget_script_run();
    set_plugin_installed(false);
    run_plugin_probe();
    settle();
    REQUIRE(scripts.script_args() == "--no-restart --relink ");
    CHECK(restarts() == restarts_before + 1);

    // An app update brings a new pair, which gets one attempt.
    ship("1.3.0", 0);
    PluginSelfHealTestAccess::set_ran_this_start(false);
    set_plugin_installed(false);
    run_plugin_probe();
    settle();
    CHECK(restarts() == restarts_before + 2);
}

TEST_CASE_METHOD(SelfHealFixture,
                 "a link re-pointed again after its restart is not restarted for at the next start",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.0.1", 3);
    const int restarts_before = restarts();
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", nullptr);

    run_plugin_probe();
    settle();
    REQUIRE(restarts() == restarts_before + 1);

    // The next app start: install.sh re-points the same link again.
    forget_script_run();
    next_start_reports_absent();
    REQUIRE(scripts.script_args() == "--no-restart --relink ");
    CHECK(restarts() == restarts_before + 1);

    // A start that finds the link current ends the wait; a later relink is new.
    ship("1.0.1", 0);
    next_start_reports_absent();
    ship("1.0.1", 3);
    next_start_reports_absent();
    CHECK(restarts() == restarts_before + 2);
}

TEST_CASE_METHOD(SelfHealFixture,
                 "the check runs once an app start, and a failed relink waits for the next",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.2.0", 1);
    const int restarts_before = restarts();
    {
        ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", "1");
        run_plugin_probe();
        settle();
    }
    REQUIRE(scripts.script_args() == "--no-restart --relink ");
    CHECK(restarts() == restarts_before);

    forget_script_run();
    set_plugin_installed(false);
    set_plugin_installed(true);
    settle();

    CHECK(scripts.script_args().empty());
    CHECK(restarts() == restarts_before);
}

TEST_CASE_METHOD(SelfHealFixture, "a remote Moonraker's plugin is only reported, never relinked",
                 "[plugin_installer][print_select][self_heal]") {
    ship("1.2.0", 3);
    PrintSelectPanelTestAccess::plugin_installer(*panel_).set_websocket_url(
        "ws://203.0.113.5:7125/websocket");
    const int restarts_before = restarts();
    ScopedEnv plugin("HELIX_MOCK_HELIX_PLUGIN", "1");

    run_plugin_probe();
    settle();

    CHECK(scripts.script_args().empty());
    CHECK(restarts() == restarts_before);
}
