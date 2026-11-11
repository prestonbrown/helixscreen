// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file timelapse_host_mock.h
 * @brief The moonraker-timelapse install a --test run simulates (HELIX_MOCK_TIMELAPSE)
 */

#include "system/process_runner.h"

#include <string>

namespace helix::mock {

/// What HELIX_MOCK_TIMELAPSE says the mock host holds.
enum class TimelapseHost {
    Present,  ///< unset: the plugin answers
    Absent,   ///< "absent": no plugin; ffmpeg is installed
    NoFfmpeg, ///< "no-ffmpeg": no plugin, no ffmpeg; the last screen shows its command
};

/// Parse a HELIX_MOCK_TIMELAPSE value, a comma-separated list of words: the
/// first of "absent" or "no-ffmpeg" picks the host; unset and unknown words are Present.
TimelapseHost parse_timelapse_host(const char* value);

/// Whether a HELIX_MOCK_TIMELAPSE value holds the word "helix-macros": Klipper
/// has HelixScreen's helper macros loaded and no TIMELAPSE_TAKE_FRAME.
bool parse_timelapse_helix_macros(const char* value);

/// parse_timelapse_helix_macros() of the environment.
bool timelapse_helix_macros_loaded();

/// Leave a stub moonraker-timelapse checkout at @p repo: its component and macro
/// file, as a clone leaves them. False when they could not be written.
bool write_stub_timelapse_checkout(const std::string& repo);

/// The mock's helix_macros.cfg while timelapse_helix_macros_loaded(): what the
/// mock file API serves and stores for that one file. Main thread only.
std::string& timelapse_mock_helix_macros_file();

/// Whether the mock's timelapse plugin answers now: from the env at start, true
/// once a simulated install linked it.
bool timelapse_plugin_present();

/// The simulated host's commands: each takes a moment and answers per
/// HELIX_MOCK_TIMELAPSE; a clone leaves a checkout at its destination, and
/// linking the component makes the plugin answer.
ProcessRunner timelapse_host_runner();

} // namespace helix::mock
