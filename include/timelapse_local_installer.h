// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file timelapse_local_installer.h
 * @brief Installs moonraker-timelapse on the host HelixScreen shares with Moonraker
 *
 * moonraker-timelapse's data for the shared local plugin installer
 * (local_plugin_installer.h), and the setup wizard's route decision. The recipe
 * does what moonraker-timelapse's own `make install` does, minus the steps that
 * need a password: clone the repository, link component/timelapse.py and
 * klipper_macro/timelapse.cfg (install.sh --link-timelapse), and check for
 * ffmpeg. A missing ffmpeg is the user's to install: the plugin records without
 * it and needs it only to render videos. The [timelapse] config sections and the Moonraker restart
 * are the setup wizard's own steps, over Moonraker's API, so the remote route shares them.
 */

#include "local_plugin_installer.h"
#include "system/process_runner.h"

#include <atomic>
#include <string>

namespace helix {

/// The repository the local install clones. The [update_manager timelapse]
/// section the setup wizard writes names the same one at ~/moonraker-timelapse.
inline constexpr const char* kTimelapseRepoUrl =
    "https://github.com/mainsail-crew/moonraker-timelapse.git";
inline constexpr const char* kTimelapseRepoDirName = "moonraker-timelapse";
/// The name of the [update_manager timelapse] section that tracks the checkout.
inline constexpr const char* kTimelapseUpdateManagerName = "timelapse";

/// What the wizard tells a user to run when ffmpeg is missing.
inline constexpr const char* kFfmpegInstallCommand = "sudo apt-get install -y ffmpeg";

/// The route the setup wizard takes once it knows whether the plugin answers.
enum class TimelapseSetupPath {
    AlreadyInstalled,   ///< the plugin answers; nothing to install
    LocalInstall,       ///< Moonraker runs on this host: HelixScreen installs the plugin
    RemoteInstructions, ///< HelixScreen cannot install it: show the SSH commands
    WaitForJob,         ///< a local install, held while a job holds the machine
};

struct TimelapseSetupFacts {
    bool plugin_present = false;      ///< the timelapse plugin answers Moonraker's API
    bool moonraker_local = false;     ///< Moonraker is known to run on this host
    bool can_install_locally = false; ///< install.sh found and a home directory to clone into
    bool job_holds = false;           ///< job_holds_machine() of the current print lifecycle
};

/**
 * @brief Pick the wizard's route
 *
 * A Moonraker whose locality is not known yet counts as remote: the SSH
 * commands are right on either host, a local install is not. A local install
 * never starts while a job holds the machine, as it ends in a Moonraker restart.
 */
TimelapseSetupPath decide_timelapse_setup_path(const TimelapseSetupFacts& facts);

/// moonraker-timelapse's macro file inside its checkout.
inline constexpr const char* kTimelapseMacroFile = "klipper_macro/timelapse.cfg";

/// The macro Klipper defines once it loads moonraker-timelapse's timelapse.cfg.
inline constexpr const char* kTimelapseFrameMacro = "TIMELAPSE_TAKE_FRAME";

/// The line a user adds to printer.cfg when HelixScreen cannot include the macros itself.
inline constexpr const char* kTimelapseIncludeLine = "[include timelapse.cfg]";

/// How moonraker-timelapse's macros get loaded into Klipper.
enum class TimelapseMacroSetup {
    NotYetKnown,            ///< Klipper's objects have not arrived: say nothing yet
    Loaded,                 ///< Klipper has TIMELAPSE_TAKE_FRAME: nothing to do
    IncludeFromHelixMacros, ///< helix_macros.cfg includes the checkout's macro file
    ShowIncludeLine,        ///< HelixScreen cannot include it: the user adds the line
};

struct TimelapseMacroFacts {
    bool objects_reported = false;    ///< Klipper's objects list arrived on this connection
    bool has_frame_macro = false;     ///< it has gcode_macro TIMELAPSE_TAKE_FRAME
    bool helix_macros_loaded = false; ///< it has HelixScreen's helper macros
    /// The checkout's macro file is known by absolute path on the Klipper host:
    /// Moonraker runs on this host and the file is there.
    bool macro_file_known = false;
};

/**
 * @brief Pick how the macros get loaded
 *
 * Decided from what Klipper has loaded, never from printer.cfg's text, so a
 * printer that already includes timelapse.cfg (directly or through another
 * file) never gets a second include.
 */
TimelapseMacroSetup decide_timelapse_macro_setup(const TimelapseMacroFacts& facts);

/// The moonraker-timelapse checkout whose macro file helix_macros.cfg may
/// include: its absolute path when Moonraker runs on this host (@p moonraker_local)
/// and klipper_macro/timelapse.cfg is in it, empty otherwise.
std::string timelapse_macro_repo_on_host(bool moonraker_local);

/// moonraker-timelapse's recipe for the local plugin installer: clone it to
/// ~/moonraker-timelapse, `install.sh --link-timelapse` links its component and
/// macro file, and it needs ffmpeg to render videos.
const LocalPluginRecipe& timelapse_plugin_recipe();

/// The home directory a local install clones under: $HOME, or empty when it is
/// unset or empty, and then the install takes the SSH route instead. Against a
/// mock Moonraker, a scratch directory the simulated clone fills.
std::string timelapse_install_home();

/// The runner a local install uses: real processes that @p cancel stops, or the
/// simulated host under --test.
ProcessRunner timelapse_install_runner(const std::atomic<bool>& cancel);

/// The same runner without a cancel: each command runs to its own timeout. For
/// the one command that must not be stopped part way (start_unless_cancelled).
ProcessRunner timelapse_install_runner_to_end();

} // namespace helix
