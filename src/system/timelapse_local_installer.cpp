// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_local_installer.h"

#include "app_globals.h"
#include "helix_fs.h"
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

TimelapseMacroSetup decide_timelapse_macro_setup(const TimelapseMacroFacts& facts) {
    if (!facts.objects_reported) {
        return TimelapseMacroSetup::NotYetKnown;
    }
    if (facts.has_frame_macro) {
        return TimelapseMacroSetup::Loaded;
    }
    return facts.helix_macros_loaded && facts.macro_file_known
               ? TimelapseMacroSetup::IncludeFromHelixMacros
               : TimelapseMacroSetup::ShowIncludeLine;
}

std::string timelapse_macro_repo_on_host(bool moonraker_local) {
    const std::string home = timelapse_install_home();
    if (!moonraker_local || home.empty() || home.front() != '/') {
        return {};
    }
    const std::string repo = home + "/" + kTimelapseRepoDirName;
    // HELIX_MOCK_TIMELAPSE=helix-macros: a plugin installed before this run, with its checkout.
    if (get_runtime_config()->should_mock_moonraker() && mock::timelapse_helix_macros_loaded() &&
        !fs::exists(repo + "/" + kTimelapseMacroFile)) {
        mock::write_stub_timelapse_checkout(repo);
    }
    return fs::exists(repo + "/" + kTimelapseMacroFile) ? repo : std::string();
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

ProcessRunner timelapse_install_runner_to_end() {
    if (get_runtime_config()->should_mock_moonraker()) {
        return mock::timelapse_host_runner();
    }
    return run_process;
}

} // namespace helix
