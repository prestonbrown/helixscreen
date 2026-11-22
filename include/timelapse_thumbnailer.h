// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <sys/types.h>
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

/// Which frame a thumbnail is taken from.
enum class ExtractFrame {
    Last,  ///< Near the end, as moonraker-timelapse's own preview is; a cheap seek
    First, ///< The opening frame, for a file the end seek gets nothing from
};

/// Build an argument list for ffmpeg thumbnail extraction (safe from shell injection)
std::vector<std::string> ffmpeg_extract_args(const std::string& input_path,
                                             const std::string& output_path, ExtractFrame frame);

/// The thumbnail cache key of a frame extracted from @p filename. Size and
/// modification time are in it, so a re-rendered video gets a fresh thumbnail.
[[nodiscard]] std::string extracted_thumbnail_key(const std::string& filename, uint64_t size,
                                                  double modified);

struct ThumbnailExtractInputs {
    bool has_companion = false;    ///< moonraker-timelapse left a preview beside the video
    bool ffmpeg_available = false; ///< ffmpeg is on this host's PATH
    bool moonraker_local = false;  ///< the video is on this host's disk
    bool print_active = false;     ///< a print holds the machine; its CPU is not ours to spend
    bool already_failed = false;   ///< extraction failed for this file before
};

/// Whether to extract a thumbnail from the video itself.
[[nodiscard]] bool should_extract_thumbnail(const ThumbnailExtractInputs& in);

bool is_video_file(const std::string& filename);

// --- Playback utilities ---

/**
 * @brief The URL a phone on the LAN downloads @p filename from
 *
 * Moonraker serves it at <base>/server/files/timelapse/<filename>, the file name
 * percent-encoded. A loopback @p http_base_url (Moonraker on this machine, reached
 * as localhost) names this machine only to itself, so its host is swapped for
 * @p lan_ipv4. Empty when the base is unusable, or loopback with no LAN address.
 */
[[nodiscard]] std::string phone_video_url(const std::string& http_base_url,
                                          const std::string& filename, const std::string& lan_ipv4);

} // namespace helix::timelapse
