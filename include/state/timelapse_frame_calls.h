// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace helix::timelapse {

/// The command moonraker-timelapse's Klipper macros define to take one frame.
inline constexpr const char* kTakeFrameCommand = "TIMELAPSE_TAKE_FRAME";

/// What a print file's scanned head says about timelapse frame calls
/// (frame_calls_in() in timelapse_frame_check.h).
enum class FrameCalls {
    Unknown, ///< Not enough to tell: macros unread, or the head ends inside the first layers
    Present, ///< A line runs a frame-taking command
    Missing, ///< The head covers a whole layer change and nothing in it takes a frame
};

} // namespace helix::timelapse
