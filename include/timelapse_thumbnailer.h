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

/// Build an argument list for video playback (safe from shell injection)
std::vector<std::string> build_player_args(const std::string& player, const std::string& file_path);

/// What a tap on a timelapse video does on this host.
enum class VideoOpenAction {
    Play,          ///< An external player exists: launch it
    PhoneQr,       ///< No player: show a QR code of the video's URL for a phone
    NoPlayerToast, ///< No player and no URL a phone could reach: say so
};

/// The one rule both entry points (the library's card tap and History) follow.
/// @p phone_url is phone_video_url()'s answer; empty means none could be built.
[[nodiscard]] VideoOpenAction choose_video_open_action(bool can_play, const std::string& phone_url);

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

/// A launched player and the watcher that reports whether it died at once.
struct PlayerWatch {
    pid_t watcher_pid = -1;
    int status_fd = -1; ///< Read end; the watcher writes the player's wait status here
};

enum class PlayerWatchState {
    Pending, ///< The watcher's window has not closed yet
    Running, ///< The player outlived the window and keeps running on its own
    Exited,  ///< The player exited inside the window; wait_status says how
};

struct PlayerWatchPoll {
    PlayerWatchState state = PlayerWatchState::Pending;
    int wait_status = 0;
};

/**
 * @brief Launch @p args detached, watched for @p window_ms
 *
 * A watcher process starts the player, waits up to the window for it, writes
 * its wait status to the pipe if it exited, and exits; a player still running
 * is adopted by init. nullopt when the processes could not be created.
 */
[[nodiscard]] std::optional<PlayerWatch> spawn_watched_player(const std::vector<std::string>& args,
                                                              int window_ms);

/// Non-blocking check on @p watch. Reaps the watcher and closes the pipe once
/// the state is no longer Pending.
[[nodiscard]] PlayerWatchPoll poll_player_watch(PlayerWatch& watch);

/// True unless @p wait_status is a normal exit with status 0.
[[nodiscard]] bool player_exit_failed(int wait_status);

/// "exit N" or "signal N", for logs and the failure toast.
[[nodiscard]] std::string describe_wait_status(int wait_status);

} // namespace helix::timelapse
