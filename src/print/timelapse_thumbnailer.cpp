// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_thumbnailer.h"

#include <algorithm>
#include <functional>

namespace helix::timelapse {

std::string cache_key(const std::string& video_filename) {
    auto hash = std::hash<std::string>{}(video_filename);
    return "tl_" + std::to_string(hash);
}

std::string companion_filename(const std::string& video_filename) {
    auto dot = video_filename.rfind('.');
    if (dot == std::string::npos)
        return video_filename + ".thumb.jpg";
    return video_filename.substr(0, dot) + ".thumb.jpg";
}

std::vector<std::string> ffmpeg_extract_args(const std::string& input_path,
                                             const std::string& output_path) {
    return {"ffmpeg", "-y", "-i", input_path, "-vframes", "1", "-q:v", "3", output_path};
}

bool is_video_file(const std::string& filename) {
    if (filename.size() < 5)
        return false;
    if (filename.find(".thumb.") != std::string::npos)
        return false;

    auto dot = filename.rfind('.');
    if (dot == std::string::npos)
        return false;
    auto ext = filename.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".mp4" || ext == ".mkv" || ext == ".avi";
}

std::vector<std::string> build_player_args(const std::string& player,
                                           const std::string& file_path) {
    if (player == "mpv") {
        return {"mpv", "--fs", "--keep-open=no", file_path};
    }
    return {"ffplay", "-autoexit", "-exitonmousedown", "-fs", file_path};
}

} // namespace helix::timelapse
