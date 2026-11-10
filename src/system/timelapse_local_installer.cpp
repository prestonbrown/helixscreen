// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_local_installer.h"

#include "app_globals.h"
#include "runtime_config.h"
#include "timelapse_host_mock.h"

#include <cstdlib>

namespace helix {

TimelapseSetupPath decide_timelapse_setup_path(const TimelapseSetupFacts& facts) {
    if (facts.plugin_present) {
        return TimelapseSetupPath::AlreadyInstalled;
    }
    if (!facts.moonraker_local || !facts.can_install_locally) {
        return TimelapseSetupPath::RemoteInstructions;
    }
    return facts.job_holds ? TimelapseSetupPath::WaitForJob : TimelapseSetupPath::LocalInstall;
}

const LocalPluginRecipe& timelapse_plugin_recipe() {
    static const LocalPluginRecipe recipe = [] {
        LocalPluginRecipe r;
        r.name = "moonraker-timelapse";
        r.repo_url = kTimelapseRepoUrl;
        r.repo_dir_name = kTimelapseRepoDirName;
        r.repo_marker = "component/timelapse.py";
        r.repo_branch = "main";
        r.install_args = {"--link-timelapse", kLocalPluginRepoDirArg};
        // install.sh walks /proc and the config; it never waits on Moonraker.
        r.install_timeout_seconds = 60;
        r.dependencies = {{"ffmpeg", "ffmpeg"}};
        return r;
    }();
    return recipe;
}

std::string timelapse_install_home() {
    // The simulated install's clone lands somewhere, and never in the real home.
    if (get_runtime_config()->should_mock_moonraker()) {
        return get_helix_cache_dir("mock_timelapse_home");
    }
    const char* home = std::getenv("HOME");
    return home ? home : "";
}

ProcessRunner timelapse_install_runner(const std::atomic<bool>& cancel) {
    // A mock Moonraker is not on this host: its install is simulated, never run here.
    if (get_runtime_config()->should_mock_moonraker()) {
        return mock::timelapse_host_runner();
    }
    return cancellable_process_runner(cancel);
}

} // namespace helix
