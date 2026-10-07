// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_preview_fetcher.h"

#include "ui_filename_utils.h"

#include "app_globals.h"
#include "config.h"
#include "gcode_preview_setup.h"
#include "i_moonraker_api.h"
#include "memory_utils.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <vector>

namespace tio = helix::text_io;
using helix::gcode::resolve_gcode_filename;

namespace helix::ui {

void GcodePreviewFetcher::fetch(const std::string& filename, ReadyCb on_ready,
                                UnavailableCb on_unavailable) {
    auto req = std::make_shared<Request>();
    req->generation = ++generation_;
    req->filename = filename;
    req->on_ready = [this, on_ready = std::move(on_ready)](const std::string& path, Source) {
        owned_path_ = path;
        if (on_ready) {
            on_ready(path);
        }
    };
    req->on_unavailable = std::move(on_unavailable);

    // Thumbnail Only skips all gcode downloading/parsing.
    if (!preview_viewer_enabled()) {
        spdlog::info("[{}] G-code render mode is Thumbnail Only - skipping G-code load", log_tag_);
        give_up(req, Unavailable::Disabled);
        return;
    }

    // Config option to disable 3D rendering entirely
    auto* cfg = Config::get_instance();
    if (!cfg->get<bool>("/display/gcode_3d_enabled", true)) {
        spdlog::info("[{}] G-code 3D rendering disabled via config - using thumbnail only",
                     log_tag_);
        give_up(req, Unavailable::Disabled);
        return;
    }

    // Persistent cache directory (not /tmp, which may be RAM-backed on embedded)
    std::string cache_dir = get_helix_cache_dir("gcode_temp");
    if (cache_dir.empty()) {
        spdlog::warn("[{}] No writable cache directory - skipping G-code preview", log_tag_);
        give_up(req, Unavailable::NoCacheDir);
        return;
    }
    req->temp_path = cache_dir + "/" + cache_file_name("print_view_", filename);

    // The file is already coming down: take over that transfer. Its bytes are
    // partial, so no cache lookup may see them either.
    if (auto it = in_flight_.find(req->temp_path); it != in_flight_.end()) {
        spdlog::debug("[{}] Joining the running download of '{}'", log_tag_, filename);
        it->second.push_back(req);
        return;
    }

    if (helix::gcode::is_3mf(filename)) {
        list_qidi_shadow(req);
        return;
    }
    // Metadata gives the size, which decides whether to download at all. This
    // prevents OOM on memory-constrained devices like AD5M.
    lookup_metadata(req, resolve_gcode_filename(filename), "gcodes", filename);
}

std::string GcodePreviewFetcher::cache_file_name(const char* prefix, const std::string& file_key) {
    auto* cfg = Config::get_instance();
    const std::string printer = cfg->get<std::string>(cfg->df() + "moonraker_host", "localhost") +
                                ":" +
                                std::to_string(cfg->get<int>(cfg->df() + "moonraker_port", 7125));
    return std::string(prefix) +
           std::to_string(std::hash<std::string>{}(printer + '\n' + file_key)) + ".gcode";
}

void GcodePreviewFetcher::discard_file() {
    if (owned_path_.empty()) {
        return;
    }
    if (std::remove(owned_path_.c_str()) == 0) {
        spdlog::debug("[{}] Cleaned up temp G-code file: {}", log_tag_, owned_path_);
    } else {
        spdlog::trace("[{}] Temp G-code file already removed: {}", log_tag_, owned_path_);
    }
    owned_path_.clear();
}

// Every callback below fires on a background thread (get_file_metadata and
// list_files on libhv's WS event loop, download_file_to_path on
// HttpExecutor::slow()). Each marshals to the main thread through the token
// before touching the fetcher or calling the owner.

// A .3mf is a zip archive the viewer cannot read; its G-code is only on show
// when the firmware extracted the printing plate into the `.temp` root. Without
// that copy there is nothing to render, and the archive itself is never
// downloaded.
void GcodePreviewFetcher::list_qidi_shadow(const RequestPtr& req) {
    auto token = lifetime_.token();
    api_->files().list_files(
        ".temp", "", false,
        [this, token, req](const std::vector<FileInfo>& files) {
            token.defer("GcodePreviewFetcher::qidi_3mf_shadow_list_ok", [this, req, files]() {
                if (stale(req)) {
                    return;
                }
                spdlog::debug("[{}] .temp returned {} entries for QIDI native 3MF preview lookup",
                              log_tag_, files.size());

                // A multi-plate .3mf can leave several shadow_native_plate_*.gcode
                // files in .temp, and Moonraker exposes no plate index for the
                // active print. The active plate's shadow is (re)written at print
                // start, so the newest-modified match is the best proxy for "the
                // plate currently printing".
                const FileInfo* best = nullptr;
                for (const auto& file : files) {
                    if (!helix::gcode::is_native_3mf_shadow(file.path) &&
                        !helix::gcode::is_qidi_3mf_extract(file.path, req->filename)) {
                        continue;
                    }
                    if (best == nullptr || file.modified > best->modified) {
                        best = &file;
                    }
                }

                if (best != nullptr) {
                    spdlog::debug("[{}] Selected QIDI native 3MF G-code (newest of matches): "
                                  ".temp/{} ({} bytes, modified {})",
                                  log_tag_, best->path, best->size, best->modified);
                    stream_if_safe(req, ".temp", best->path, best->size);
                    return;
                }

                spdlog::info("[{}] No extracted G-code for '{}' in .temp - keeping the thumbnail",
                             log_tag_, req->filename);
                give_up(req, Unavailable::NoGcode);
            });
        },
        [this, token, req](const MoonrakerError& err) {
            token.defer("GcodePreviewFetcher::qidi_3mf_shadow_list_err", [this, req, err]() {
                if (stale(req)) {
                    return;
                }
                spdlog::info("[{}] Cannot list .temp for '{}': {} - keeping the thumbnail",
                             log_tag_, req->filename, err.message);
                give_up(req, Unavailable::NoGcode);
            });
        });
}

void GcodePreviewFetcher::lookup_metadata(const RequestPtr& req, const std::string& metadata_target,
                                          const std::string& root,
                                          const std::string& download_target) {
    auto token = lifetime_.token();
    api_->files().get_file_metadata(
        metadata_target,
        [this, token, req, root, download_target](const FileMetadata& metadata) {
            token.defer("GcodePreviewFetcher::metadata_ok",
                        [this, req, root, download_target, size = metadata.size]() {
                            if (stale(req)) {
                                return;
                            }
                            stream_if_safe(req, root, download_target, size);
                        });
        },
        [this, token, req](const MoonrakerError& err) {
            token.defer("GcodePreviewFetcher::metadata_err", [this, req, err]() {
                if (stale(req)) {
                    return;
                }
                // Metadata only decides whether we need to DOWNLOAD the file. If
                // the owner already renders it, or a cached copy exists (size
                // unknown, so any non-empty copy is trusted), a metadata miss must
                // not blank the preview. Reachable on a flaky link or while
                // Moonraker is rescanning, and the error is silent (no toast), so
                // giving up here would leave a blank preview for the rest of the
                // print.
                if (rendered_probe_ && rendered_probe_()) {
                    spdlog::debug("[{}] G-code metadata unavailable for '{}': {} - keeping "
                                  "already-loaded render",
                                  log_tag_, req->filename, err.message);
                    return;
                }
                const size_t cached_size =
                    static_cast<size_t>(tio::file_size(req->temp_path).value_or(0));
                if (preview_cache_is_current(cached_size, 0)) {
                    if (helix::is_gcode_2d_streaming_safe(cached_size)) {
                        spdlog::info("[{}] G-code metadata unavailable for '{}': {} - using "
                                     "cached copy ({} bytes)",
                                     log_tag_, req->filename, err.message, cached_size);
                        hand_over(req, req->temp_path, Source::Cache);
                        return;
                    }
                    std::remove(req->temp_path.c_str());
                }
                spdlog::debug(
                    "[{}] Failed to get G-code metadata for '{}': {} - skipping 3D render",
                    log_tag_, req->filename, err.message);
                give_up(req, Unavailable::MetadataFailed);
            });
        },
        true // silent - don't trigger RPC_ERROR event/toast
    );
}

void GcodePreviewFetcher::stream_if_safe(const RequestPtr& req, const std::string& root,
                                         const std::string& download_target, uint64_t size) {
    if (!helix::is_gcode_2d_streaming_safe(size)) {
        auto mem = helix::get_system_memory_info();
        spdlog::warn("[{}] G-code too large for 2D streaming: file={} bytes, available RAM={}MB - "
                     "using thumbnail only",
                     log_tag_, size, mem.available_mb());
        give_up(req, Unavailable::TooLarge);
        return;
    }

    ensure_local(req, root, download_target, size);
}

void GcodePreviewFetcher::ensure_local(const std::string& root, const std::string& remote_path,
                                       const std::string& local_path, uint64_t expected_bytes,
                                       LocalReadyCb on_ready, UnavailableCb on_unavailable) {
    auto req = std::make_shared<Request>();
    req->generation = generation_;
    req->filename = remote_path;
    req->temp_path = local_path;
    req->on_ready = std::move(on_ready);
    req->on_unavailable = std::move(on_unavailable);
    ensure_local(req, root, remote_path, expected_bytes);
}

void GcodePreviewFetcher::ensure_local(const RequestPtr& req, const std::string& root,
                                       const std::string& remote_path, uint64_t expected_bytes) {
    // A transfer is already running: join it. Checked BEFORE the disk probe, as
    // the file is partially written and a non-empty copy must not be mistaken for
    // a complete one.
    if (auto it = in_flight_.find(req->temp_path); it != in_flight_.end()) {
        it->second.push_back(req);
        return;
    }

    // The cache is keyed by file name alone; the size says whether the copy is
    // still the file on the server. A mismatch means it was re-sliced onto the
    // same path or a transfer was cut short, and scanning or rendering those
    // bytes would show the wrong print.
    const size_t on_disk = static_cast<size_t>(tio::file_size(req->temp_path).value_or(0));
    if (on_disk > 0) {
        if (preview_cache_is_current(on_disk, expected_bytes)) {
            spdlog::info("[{}] Using cached G-code file ({} bytes): {}", log_tag_, on_disk,
                         req->temp_path);
            hand_over(req, req->temp_path, Source::Cache);
            return;
        }
        spdlog::warn("[{}] Cached G-code size mismatch (disk={}, expected={}) - re-downloading",
                     log_tag_, on_disk, expected_bytes);
        std::remove(req->temp_path.c_str());
    }

    in_flight_[req->temp_path].push_back(req);

    if (!owned_path_.empty() && owned_path_ != req->temp_path) {
        std::remove(owned_path_.c_str());
        owned_path_.clear();
    }

    // The completions are not stale-checked against `req`: the transfer outlives
    // a cancel, and whichever requests wait on it now are the ones to tell.
    auto token = lifetime_.token();
    const std::string temp_path = req->temp_path;
    api_->transfers().download_file_to_path(
        root, remote_path, temp_path,
        [this, token, temp_path](const std::string& path) {
            token.defer("GcodePreviewFetcher::download_ok", [this, temp_path, path]() {
                bool delivered = false;
                for (const auto& waiter : take_waiters(temp_path)) {
                    if (stale(waiter)) {
                        continue;
                    }
                    if (!delivered) {
                        spdlog::debug("[{}] Streamed G-code to disk: {}", log_tag_, path);
                    }
                    delivered = true;
                    hand_over(waiter, path, Source::Download);
                }
                // Nobody wants the copy and nothing tracks it for cleanup.
                if (!delivered && owned_path_ != path) {
                    std::remove(path.c_str());
                }
            });
        },
        [this, token, temp_path](const MoonrakerError& err) {
            token.defer("GcodePreviewFetcher::download_err", [this, temp_path, err]() {
                for (const auto& waiter : take_waiters(temp_path)) {
                    if (stale(waiter)) {
                        continue;
                    }
                    spdlog::warn("[{}] Failed to stream G-code '{}': {}", log_tag_,
                                 waiter->filename, err.message);
                    give_up(waiter, Unavailable::DownloadFailed);
                }
            });
        });
}

std::vector<GcodePreviewFetcher::RequestPtr>
GcodePreviewFetcher::take_waiters(const std::string& temp_path) {
    std::vector<RequestPtr> waiters;
    if (auto it = in_flight_.find(temp_path); it != in_flight_.end()) {
        waiters = std::move(it->second);
        in_flight_.erase(it);
    }
    return waiters;
}

void GcodePreviewFetcher::hand_over(const RequestPtr& req, const std::string& path, Source source) {
    if (req->on_ready) {
        req->on_ready(path, source);
    }
}

void GcodePreviewFetcher::give_up(const RequestPtr& req, Unavailable why) {
    if (req->on_unavailable) {
        req->on_unavailable(why);
    }
}

} // namespace helix::ui
