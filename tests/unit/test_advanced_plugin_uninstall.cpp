// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_advanced_plugin_uninstall.cpp
 * @brief The Advanced panel's Uninstall HelixPrint Plugin row
 *
 * The row asks for confirmation and only then runs the uninstaller, which
 * removes the plugin's Moonraker component (helix_macros.cfg is kept) and
 * strips any old phase-tracking lines from PRINT_START. These cases pin that
 * the uninstaller runs on the confirm button and on nothing else (cancel,
 * backdrop dismissal), and that helix_plugin_installed clears on both
 * SUCCESS and NEEDS_ATTENTION - the plugin is gone either way - and only a
 * FAILED run leaves it alone.
 *
 * Both install and uninstall restart Moonraker over its API once the script
 * succeeds: the script runs under the service's NoNewPrivileges and cannot
 * sudo, so without that restart Moonraker never loads or drops the plugin.
 * That restart is why both rows refuse, with a warning, while a job holds the
 * machine.
 */

#include "ui_modal.h"
#include "ui_panel_advanced.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/advanced_panel_test_access.h"
#include "../test_helpers/plugin_script_sandbox.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "printer_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::AdvancedPanelTestAccess;
using helix::ui::UpdateQueue;

namespace {

struct PluginUninstallFixture : LVGLUITestFixture {
    using Outcome = helix::UninstallOutcome;

    AdvancedPanel panel{get_printer_state(), nullptr};

    int uninstall_runs = 0;
    int restarts = 0;
    Outcome uninstall_outcome = Outcome::SUCCESS;

    PluginUninstallFixture() {
        helix::ui::modal_init_subjects();

        // The recorder answers synchronously, the way the real installer does
        // once the script has exited.
        AdvancedPanelTestAccess::set_uninstall_runner(
            panel, [this](helix::HelixPluginInstaller::UninstallCallback done) {
                ++uninstall_runs;
                const char* message = uninstall_outcome == Outcome::SUCCESS ? "removed"
                                      : uninstall_outcome == Outcome::NEEDS_ATTENTION
                                          ? "removed, config needs a look"
                                          : "script failed";
                done(uninstall_outcome, message);
            });

        AdvancedPanelTestAccess::set_moonraker_restarter(panel, [this]() { ++restarts; });

        // The row is only reachable while the plugin is installed.
        get_printer_state().set_helix_plugin_installed(true);
        UpdateQueue::instance().drain();
        REQUIRE(plugin_installed() == 1);
    }

    ~PluginUninstallFixture() override {
        helix::test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
        UpdateQueue::instance().drain();
    }

    std::vector<std::string> warnings;

    void start_print() {
        helix::ui::set_test_notification_warning_hook(
            [this](const std::string& message) { warnings.push_back(message); });
        helix::test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
        UpdateQueue::instance().drain();
        REQUIRE(job_holds_machine(get_printer_state().print_state().get_print_lifecycle()));
    }

    bool warned_about_printing() const {
        for (const auto& w : warnings) {
            if (w.find("while printing") != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    int plugin_installed() {
        return lv_subject_get_int(
            get_printer_state().plugin_status_state().get_helix_plugin_installed_subject());
    }

    void tap_row() {
        AdvancedPanelTestAccess::tap_uninstall_row(panel);
        process_lvgl(50);
    }

    /// Answer the confirmation by clicking one of its buttons; the modal
    /// closes itself and the queued follow-ups need the drain.
    void answer_modal(const char* button_name) {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_t* button = lv_obj_find_by_name(dialog, button_name);
        REQUIRE(button != nullptr);
        lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
        process_lvgl(50);
        UpdateQueue::instance().drain();
    }

    /// The install row, then the modal's Install button, with a stub install.sh
    /// running @p script_body. Returns the result text the modal shows.
    std::string install_with_script(const std::string& script_body) {
        helix::test::PluginScriptSandbox scripts(AdvancedPanelTestAccess::plugin_installer(panel));
        scripts.write_script(script_body);
        AdvancedPanelTestAccess::tap_install_row(panel);
        process_lvgl(50);
        answer_modal("btn_install");
        REQUIRE(scripts.script_args() == "--no-restart --auto ");
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_t* message = lv_obj_find_by_name(dialog, "result_message");
        REQUIRE(message != nullptr);
        return lv_label_get_text(message);
    }

    /// A backdrop tap: the dismissal path no button answers.
    void dismiss_modal() {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
        REQUIRE(backdrop != nullptr);
        lv_obj_send_event(backdrop, LV_EVENT_CLICKED, nullptr);
        process_lvgl(50);
        UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(PluginUninstallFixture, "uninstall row asks before it runs anything",
                 "[advanced][plugin_installer][1232]") {
    tap_row();

    CHECK(uninstall_runs == 0);
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    CHECK(plugin_installed() == 1);
}

TEST_CASE_METHOD(PluginUninstallFixture, "confirming the uninstall runs it and clears installed",
                 "[advanced][plugin_installer][1232]") {
    tap_row();
    answer_modal("btn_primary");

    CHECK(uninstall_runs == 1);
    CHECK(plugin_installed() == 0);
    CHECK(restarts == 1);
    CHECK(ModalStack::instance().stack_empty());
}

TEST_CASE_METHOD(PluginUninstallFixture, "cancelling the uninstall runs nothing",
                 "[advanced][plugin_installer][1232]") {
    tap_row();
    answer_modal("btn_secondary");

    CHECK(uninstall_runs == 0);
    CHECK(plugin_installed() == 1);
    CHECK(restarts == 0);
    CHECK(ModalStack::instance().stack_empty());
}

TEST_CASE_METHOD(PluginUninstallFixture, "dismissing the uninstall dialog runs nothing",
                 "[advanced][plugin_installer][1232]") {
    tap_row();
    dismiss_modal();

    CHECK(uninstall_runs == 0);
    CHECK(plugin_installed() == 1);
    CHECK(ModalStack::instance().stack_empty());
}

TEST_CASE_METHOD(PluginUninstallFixture, "a failed uninstall leaves the plugin marked installed",
                 "[advanced][plugin_installer][1232]") {
    uninstall_outcome = Outcome::FAILED;

    tap_row();
    answer_modal("btn_primary");

    CHECK(uninstall_runs == 1);
    CHECK(plugin_installed() == 1);
    CHECK(restarts == 0);
}

TEST_CASE_METHOD(PluginUninstallFixture, "a needs-attention uninstall still clears installed",
                 "[advanced][plugin_installer][1232]") {
    // The plugin's Moonraker component is gone either way; NEEDS_ATTENTION
    // only means a config file needs a human look, not that the plugin is
    // still there.
    uninstall_outcome = Outcome::NEEDS_ATTENTION;

    tap_row();
    answer_modal("btn_primary");

    CHECK(uninstall_runs == 1);
    CHECK(plugin_installed() == 0);
    CHECK(restarts == 1);
}

TEST_CASE_METHOD(PluginUninstallFixture,
                 "a successful install restarts Moonraker and leaves installed to discovery",
                 "[advanced][plugin_installer]") {
    get_printer_state().set_helix_plugin_installed(false);
    UpdateQueue::instance().drain();
    REQUIRE(plugin_installed() == 0);

    const std::string message = install_with_script("exit 0");

    CHECK(restarts == 1);
    CHECK(message.find("restarting") != std::string::npos);
    // Files on disk are not a loaded plugin: the reconnect's check decides.
    CHECK(plugin_installed() == 0);
}

TEST_CASE_METHOD(PluginUninstallFixture, "a failed install restarts nothing",
                 "[advanced][plugin_installer]") {
    get_printer_state().set_helix_plugin_installed(false);
    UpdateQueue::instance().drain();

    const std::string message = install_with_script("exit 1");

    CHECK(restarts == 0);
    CHECK(message.find("restarting") == std::string::npos);
    CHECK(plugin_installed() == 0);
}

TEST_CASE_METHOD(PluginUninstallFixture, "the uninstall row refuses while a job holds the machine",
                 "[advanced][plugin_installer]") {
    start_print();

    tap_row();

    CHECK(ModalStack::instance().stack_empty());
    CHECK(uninstall_runs == 0);
    CHECK(restarts == 0);
    CHECK(plugin_installed() == 1);
    CHECK(warned_about_printing());
}

TEST_CASE_METHOD(PluginUninstallFixture,
                 "a job that starts while the uninstall confirmation is open stops the uninstall",
                 "[advanced][plugin_installer]") {
    tap_row();
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    start_print();

    answer_modal("btn_primary");

    CHECK(uninstall_runs == 0);
    CHECK(restarts == 0);
    CHECK(plugin_installed() == 1);
    CHECK(warned_about_printing());
}

TEST_CASE_METHOD(PluginUninstallFixture, "the install row refuses while a job holds the machine",
                 "[advanced][plugin_installer]") {
    get_printer_state().set_helix_plugin_installed(false);
    UpdateQueue::instance().drain();
    start_print();

    AdvancedPanelTestAccess::tap_install_row(panel);
    process_lvgl(50);

    CHECK(ModalStack::instance().stack_empty());
    CHECK(restarts == 0);
    CHECK(warned_about_printing());
}

TEST_CASE_METHOD(PluginUninstallFixture,
                 "a job that starts while the install modal is open refuses the install",
                 "[advanced][plugin_installer]") {
    get_printer_state().set_helix_plugin_installed(false);
    UpdateQueue::instance().drain();
    helix::test::PluginScriptSandbox scripts(AdvancedPanelTestAccess::plugin_installer(panel));
    scripts.write_script("exit 0");
    AdvancedPanelTestAccess::tap_install_row(panel);
    process_lvgl(50);
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    start_print();

    answer_modal("btn_install");

    CHECK(scripts.script_args().empty());
    CHECK(restarts == 0);
    CHECK(warned_about_printing());
}

TEST_CASE_METHOD(PluginUninstallFixture, "the plugin overview row shows only while it is installed",
                 "[advanced][plugin]") {
    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "advanced_panel", nullptr));
    REQUIRE(root != nullptr);
    lv_obj_t* row = lv_obj_find_by_name(root, "row_helix_plugin_overview");
    REQUIRE(row != nullptr);
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    get_printer_state().set_helix_plugin_installed(false);
    UpdateQueue::instance().drain();
    REQUIRE(plugin_installed() != 1);
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(root);
}

// The phase-tracking toggle stays out of the app (#1234). Each case first proves
// its setup ran, so the absence assertions cannot pass on a setup that did nothing.
TEST_CASE_METHOD(PluginUninstallFixture, "advanced panel builds without a phase tracking row",
                 "[advanced][plugin][1234]") {
    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "advanced_panel", nullptr));
    REQUIRE(root != nullptr);

    // Positive controls: the panel's plugin and macro rows all built.
    REQUIRE(lv_obj_find_by_name(root, "row_helix_plugin_install") != nullptr);
    REQUIRE(lv_obj_find_by_name(root, "row_helix_macros_install") != nullptr);
    REQUIRE(lv_obj_find_by_name(root, "row_helix_macros_update") != nullptr);

    CHECK(lv_obj_find_by_name(root, "row_phase_tracking") == nullptr);
    CHECK(lv_obj_find_by_name(root, "container_phase_tracking") == nullptr);

    lv_obj_delete(root);
}

TEST_CASE_METHOD(PluginUninstallFixture, "no phase_tracking_enabled subject is registered",
                 "[plugin][1234]") {
    // The scope is live and holds the plugin subjects...
    REQUIRE(lv_xml_get_subject(nullptr, "helix_plugin_installed") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "helix_macros_status") != nullptr);

    // ...and the phase-tracking subject is not among them.
    CHECK(lv_xml_get_subject(nullptr, "phase_tracking_enabled") == nullptr);
}
