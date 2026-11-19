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
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plugin_install_modal_test_access.h"
#include "../test_helpers/plugin_script_sandbox.h"
#include "../ui_test_utils.h"
#include "helix_plugin_installer.h"

#include <functional>
#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PluginInstallModalTestAccess;
using helix::PluginInstallState;

namespace {

/// A modal wired to a sandboxed installer whose work is held until the test runs it.
struct InstallRig {
    helix::HelixPluginInstaller installer;
    helix::test::PluginScriptSandbox sandbox{installer};
    std::unique_ptr<PluginInstallModal> modal = std::make_unique<PluginInstallModal>();
    std::vector<std::function<void()>> submitted;
    int restarts = 0;
    int toasts = 0;

    InstallRig() {
        helix::ui::set_test_toast_hook(
            [this](ToastSeverity, const std::string&, uint32_t) { ++toasts; });
        sandbox.write_script("exit 0");
        modal->set_installer(&installer);
        modal->set_moonraker_restarter([this]() {
            ++restarts;
            return true;
        });
        PluginInstallModalTestAccess::set_install_submitter(
            *modal, [this](std::function<void()> work) { submitted.push_back(std::move(work)); });
    }
    ~InstallRig() {
        helix::ui::set_test_toast_hook(nullptr);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "PluginInstallModal runs install.sh off the tap and finishes on a later turn",
                 "[plugin_installer][plugin_install_modal]") {
    InstallRig rig;

    PluginInstallModalTestAccess::click_install(*rig.modal);

    // The tap only hands the script to a worker: nothing has run or finished.
    REQUIRE(rig.submitted.size() == 1);
    CHECK(rig.sandbox.script_args().empty());
    CHECK(rig.installer.get_state() == PluginInstallState::INSTALLING);
    CHECK(rig.restarts == 0);

    SECTION("a second tap while it runs starts nothing") {
        PluginInstallModalTestAccess::click_install(*rig.modal);
        CHECK(rig.submitted.size() == 1);
    }

    SECTION("the outcome lands on the main thread after the worker ends") {
        rig.submitted.front()();
        CHECK(rig.sandbox.script_args() == "--no-restart --auto ");
        CHECK(rig.restarts == 0);

        helix::ui::UpdateQueue::instance().drain();
        CHECK(rig.installer.get_state() == PluginInstallState::SUCCESS);
        CHECK(rig.restarts == 1);
        // Never shown, so there is no dialog for the result: it is a toast.
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
