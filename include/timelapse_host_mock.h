// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file timelapse_host_mock.h
 * @brief The moonraker-timelapse install a --test run simulates (HELIX_MOCK_TIMELAPSE)
 */

#include "system/process_runner.h"

namespace helix::mock {

/// What HELIX_MOCK_TIMELAPSE says the mock host holds.
enum class TimelapseHost {
    Present,  ///< unset: the plugin answers
    Absent,   ///< "absent": no plugin; ffmpeg is installed
    NoFfmpeg, ///< "no-ffmpeg": no plugin, no ffmpeg; the last screen shows its command
};

/// Parse a HELIX_MOCK_TIMELAPSE value; unset and unknown values are Present.
TimelapseHost parse_timelapse_host(const char* value);

/// Whether the mock's timelapse plugin answers now: from the env at start, true
/// once a simulated install linked it.
bool timelapse_plugin_present();

/// The simulated host's commands: each takes a moment and answers per
/// HELIX_MOCK_TIMELAPSE; linking the component makes the plugin answer.
ProcessRunner timelapse_host_runner();

} // namespace helix::mock
