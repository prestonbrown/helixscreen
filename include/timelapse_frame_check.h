// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/**
 * @file timelapse_frame_check.h
 * @brief Whether a print file will give moonraker-timelapse's Layer mode any frames.
 *
 * Layer mode ("layermacro") captures a frame only when the print runs
 * TIMELAPSE_TAKE_FRAME, which the slicer's layer-change G-code normally calls.
 * A file sliced without that line records next to nothing. These functions
 * read the head of a file the print-start scan already downloaded; they never
 * read a whole file.
 */

#include "state/timelapse_frame_calls.h"

#include <set>
#include <string>
#include <string_view>

namespace helix::timelapse {

/**
 * @brief Read a file head for frame calls.
 *
 * Present when a line's command word is one of @p frame_commands. Missing only
 * when the head also reaches an extruding move after the second layer-change
 * marker, so the slicer's per-layer code for at least one complete layer change
 * lies inside it wherever the slicer places that code (before or after the
 * change, at the start or the end of a layer). Anything shorter is Unknown.
 *
 * @param head            The downloaded head of the file
 * @param truncated       The download stopped at its byte limit, so the last
 *                        line may be cut and is not read
 * @param frame_commands  Upper-case command words that take a frame
 *                        (frame_taking_commands())
 */
[[nodiscard]] FrameCalls frame_calls_in(std::string_view head, bool truncated,
                                        const std::set<std::string>& frame_commands);

/**
 * @brief The commands that take a frame on this printer.
 *
 * TIMELAPSE_TAKE_FRAME itself and every macro that runs it, directly or through
 * macros it calls, so a slicer line calling the printer's own layer macro counts.
 * Empty when the printer's macros have not been read.
 */
[[nodiscard]] std::set<std::string> frame_taking_commands();

} // namespace helix::timelapse
