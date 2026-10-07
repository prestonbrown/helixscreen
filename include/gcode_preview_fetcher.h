// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file gcode_preview_fetcher.h
 * @brief Gets a print's G-code onto local disk for the preview viewer.
 *
 * Acquisition only: the render-mode gates, the cache path, the QIDI native .3mf
 * shadow lookup, the metadata size check, the streaming-safety check and the
 * download. It touches no widget; the owner turns the two outcomes into
 * display changes.
 *
 * @threading Main thread only. Every network callback marshals back through a
 *            LifetimeToken before touching the fetcher, so a callback that
 *            outlives it (or a cancel()) is dropped.
 */

#pragma once

#include "async_lifetime_guard.h"
#include "i_moonraker_api.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace helix::ui {

class GcodePreviewFetcher {
  public:
    /// Why no local copy will be handed over.
    enum class Unavailable {
        Disabled,       ///< Thumbnail Only, or 3D rendering switched off
        NoCacheDir,     ///< nowhere writable to put the copy
        TooLarge,       ///< the file would not fit the device's memory to render
        MetadataFailed, ///< the size lookup failed and no cached copy could stand in
        NoGcode,        ///< a .3mf whose extracted G-code the printer does not expose
        DownloadFailed
    };

    /// @p path is a complete local copy, safe to load.
    using ReadyCb = std::function<void(const std::string& path)>;
    /// Where a local copy came from.
    enum class Source { Cache, Download };
    using LocalReadyCb = std::function<void(const std::string& path, Source source)>;
    using UnavailableCb = std::function<void(Unavailable why)>;

    /// @p log_tag prefixes this object's log lines.
    explicit GcodePreviewFetcher(std::string log_tag) : log_tag_(std::move(log_tag)) {}

    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

    /// Asked when a size lookup fails: true when the owner already shows a
    /// render of the file, so the failure must not blank it.
    void set_rendered_probe(std::function<bool()> probe) {
        rendered_probe_ = std::move(probe);
    }

    /// Get @p filename's G-code onto disk. Exactly one of the callbacks runs
    /// later on the main thread, except when a size lookup fails while the
    /// owner already shows a render (neither runs), or the fetch is cancelled
    /// or superseded first (neither runs).
    ///
    /// Starting a fetch supersedes the one before it. A file already being
    /// downloaded is joined, never downloaded twice: the new fetch takes over
    /// the running transfer's callbacks.
    void fetch(const std::string& filename, ReadyCb on_ready, UnavailableCb on_unavailable);

    /// Put @p remote_path of @p root at @p local_path and call back with it.
    /// An existing copy is used when it is non-empty and, when @p expected_bytes
    /// is known (non-zero), exactly that size; any other copy is deleted and
    /// downloaded again. A download already running to @p local_path is joined
    /// rather than repeated, so every caller waiting on it is told when it lands.
    /// @p local_path must be a cache file this fetcher may delete.
    void ensure_local(const std::string& root, const std::string& remote_path,
                      const std::string& local_path, uint64_t expected_bytes, LocalReadyCb on_ready,
                      UnavailableCb on_unavailable);

    /// Is a download to @p local_path still running?
    bool is_downloading(const std::string& local_path) const {
        return in_flight_.count(local_path) != 0;
    }

    /// Drop every fetch still waiting on the network. A running transfer is not
    /// aborted and may finish writing its file; a later fetch of that file
    /// joins it.
    void cancel() {
        ++generation_;
    }

    /// Name of the cache file for @p file_key, e.g. "print_view_<hash>.gcode".
    /// Keyed by the connected printer's address as well as @p file_key: a file of
    /// the same name on another printer is a different file, and a copy of it
    /// can match the new one's size.
    static std::string cache_file_name(const char* prefix, const std::string& file_key);

    /// Does the fetcher hold a local copy it will delete?
    bool owns_file() const {
        return !owned_path_.empty();
    }

    /// Delete the local copy this fetcher handed over, if any.
    void discard_file();

  private:
    struct Request {
        uint64_t generation = 0;
        std::string filename;
        std::string temp_path;
        LocalReadyCb on_ready;
        UnavailableCb on_unavailable;
    };
    using RequestPtr = std::shared_ptr<Request>;

    /// A request whose fetch was cancelled or superseded.
    bool stale(const RequestPtr& req) const {
        return req->generation != generation_;
    }

    void list_qidi_shadow(const RequestPtr& req);
    void lookup_metadata(const RequestPtr& req, const std::string& metadata_target,
                         const std::string& root, const std::string& download_target);
    void stream_if_safe(const RequestPtr& req, const std::string& root,
                        const std::string& download_target, uint64_t size);
    void ensure_local(const RequestPtr& req, const std::string& root,
                      const std::string& remote_path, uint64_t expected_bytes);
    /// Remove and return the requests waiting on the download to @p temp_path.
    std::vector<RequestPtr> take_waiters(const std::string& temp_path);
    void hand_over(const RequestPtr& req, const std::string& path, Source source);
    void give_up(const RequestPtr& req, Unavailable why);

    std::string log_tag_;
    IMoonrakerAPI* api_ = nullptr;
    std::function<bool()> rendered_probe_;
    /// The copy handed to the owner, deleted when replaced or discarded.
    std::string owned_path_;
    uint64_t generation_ = 0;
    /// Downloads still running, by local path, each with the requests its
    /// completion is delivered to.
    std::map<std::string, std::vector<RequestPtr>> in_flight_;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix::ui
