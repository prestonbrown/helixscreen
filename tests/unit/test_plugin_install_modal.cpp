// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_plugin_install_modal.cpp
 * @brief The install modal runs install.sh off the main thread
 *
 * install.sh runs in a temp dir (PluginScriptSandbox), never the repository's
 * script, which edits moonraker.conf.
 */

#include "ui_plugin_install_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/plugin_script_sandbox.h"
#include "../ui_test_utils.h"
#include "core/update_queue.h"
#include "helix_plugin_installer.h"

#include <functional>
#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PluginInstallState;

namespace {

/// A shown install dialog wired to a sandboxed installer whose work is held
/// until the test runs it.
struct InstallRig {
    helix::HelixPluginInstaller installer;
    helix::test::PluginScriptSandbox sandbox{installer};
    std::unique_ptr<PluginInstallModal> modal = std::make_unique<PluginInstallModal>();
    std::vector<std::function<void()>> submitted;
    int restarts = 0;
    int toasts = 0;

    explicit InstallRig(lv_obj_t* screen) {
        helix::ui::set_test_toast_hook(
            [this](ToastSeverity, const std::string&, uint32_t) { ++toasts; });
        sandbox.write_script("exit 0");
        modal->set_installer(&installer);
        modal->set_moonraker_restarter([this]() {
            ++restarts;
            return true;
        });
        modal->set_install_submitter(
            [this](std::function<void()> work) { submitted.push_back(std::move(work)); });
        REQUIRE(modal->show(screen));
        helix::ui::UpdateQueue::instance().drain();
    }
    ~InstallRig() {
        helix::ui::set_test_toast_hook(nullptr);
    }

    /// A widget of the open dialog, by its XML name.
    static lv_obj_t* widget(const char* name) {
        lv_obj_t* found = lv_obj_find_by_name(Modal::get_top(), name);
        REQUIRE(found != nullptr);
        return found;
    }
    static bool shown(const char* name) {
        return !lv_obj_is_hidden(widget(name));
    }
    /// What a tap on the button does, as LVGL delivers it.
    static void tap(const char* name) {
        lv_obj_send_event(widget(name), LV_EVENT_CLICKED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }
    /// The worker's part alone: its outcome waits for the main thread's next turn.
    void run_worker() {
        submitted.front()();
    }
};

} // namespace

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "PluginInstallModal runs install.sh off the Install button and finishes on a later turn",
    "[plugin_installer][plugin_install_modal]") {
    InstallRig rig(test_screen());
    REQUIRE(Modal::get_top() != nullptr);
    REQUIRE(InstallRig::shown("local_button_row"));

    InstallRig::tap("btn_install");

    // The tap only hands the script to a worker: nothing has run or finished.
    REQUIRE(rig.submitted.size() == 1);
    CHECK(rig.sandbox.script_args().empty());
    CHECK(rig.installer.get_state() == PluginInstallState::INSTALLING);
    CHECK(rig.restarts == 0);
    CHECK(InstallRig::shown("installing_container"));
    // The dialog's phase hides the row that holds the Install button.
    CHECK_FALSE(InstallRig::shown("local_button_row"));

    SECTION("a second tap while it runs starts nothing") {
        InstallRig::tap("btn_install");
        CHECK(rig.submitted.size() == 1);
    }

    SECTION("the outcome lands in the dialog on the main thread after the worker ends") {
        rig.run_worker();
        CHECK(rig.sandbox.script_args() == "--no-restart --auto ");
        CHECK(rig.restarts == 0);
        CHECK_FALSE(InstallRig::shown("result_container"));

        helix::ui::UpdateQueue::instance().drain();
        CHECK(rig.installer.get_state() == PluginInstallState::SUCCESS);
        CHECK(rig.restarts == 1);
        CHECK(InstallRig::shown("result_container"));
        CHECK(InstallRig::shown("btn_ok"));
        // The dialog says the result; no toast repeats it.
        CHECK(rig.toasts == 0);
    }

    SECTION("a dialog closed while install.sh runs gets the outcome as a toast") {
        Modal::hide(Modal::get_top());
        rig.run_worker();
        CHECK(rig.restarts == 0);

        helix::ui::UpdateQueue::instance().drain();
        CHECK(rig.installer.get_state() == PluginInstallState::SUCCESS);
        CHECK(rig.restarts == 1);
        CHECK(rig.toasts == 1);
    }

    SECTION("a modal destroyed mid-install drops the outcome") {
        auto work = rig.submitted.front();
        rig.modal.reset();
        work();
        helix::ui::UpdateQueue::instance().drain();
        CHECK(rig.restarts == 0);
        CHECK(rig.toasts == 0);
        CHECK(rig.installer.get_state() == PluginInstallState::INSTALLING);
    }
}
