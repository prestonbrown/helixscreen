// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace helix::timelapse {

// --- Thumbnail utilities ---

std::string cache_key(const std::string& video_filename);
std::string companion_filename(const std::string& video_filename);

/// Build an argument list for ffmpeg thumbnail extraction (safe from shell injection)
std::vector<std::string> ffmpeg_extract_args(const std::string& input_path,
                                             const std::string& output_path);

bool is_video_file(const std::string& filename);

// --- Playback utilities ---

/// Check if a Moonraker host is the local machine

/// Build an argument list for video playback (safe from shell injection)
std::vector<std::string> build_player_args(const std::string& player, const std::string& file_path);

} // namespace helix::timelapse
