// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <set>
#include <string>
#include <vector>

namespace helix::timelapse {

// --- Thumbnail utilities ---

/**
 * @brief The preview image beside @p video_filename in a timelapse listing
 *
 * moonraker-timelapse renders its preview as <name>.jpg next to <name>.mp4;
 * <name>.thumb.jpg is the older companion name. The plugin's wins when both are
 * in @p listing (the file names of the same directory). Empty when neither is.
 */
std::string find_companion_thumbnail(const std::string& video_filename,
                                     const std::set<std::string>& listing);

/// Build an argument list for ffmpeg thumbnail extraction (safe from shell injection)
std::vector<std::string> ffmpeg_extract_args(const std::string& input_path,
                                             const std::string& output_path);

bool is_video_file(const std::string& filename);

// --- Playback utilities ---

/// Build an argument list for video playback (safe from shell injection)
std::vector<std::string> build_player_args(const std::string& player, const std::string& file_path);

} // namespace helix::timelapse
