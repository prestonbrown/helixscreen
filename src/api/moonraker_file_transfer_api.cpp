// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_file_transfer_api.h"

#include "ui_error_reporting.h"
#include "ui_notification.h"

#include "core/gcode_filename.h"
#include "core/http_executor.h"
#include "core/text_io.h"
#include "http_request_epoch.h"
#include "hv/hfile.h"
#include "hv/hurl.h"
#include "hv/requests.h"
#include "memory_monitor.h"
#include "moonraker_api_internal.h"
#include "spdlog/spdlog.h"
#include "thumbnail_rules.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

using namespace moonraker_internal;

// ============================================================================
// MoonrakerFileTransferAPI — Constructor / Destructor
// ============================================================================

MoonrakerFileTransferAPI::MoonrakerFileTransferAPI(helix::IMoonrakerClient& client,
                                                   const std::string& http_base_url)
    : client_(client), http_base_url_(http_base_url) {}

MoonrakerFileTransferAPI::~MoonrakerFileTransferAPI() = default;

// ============================================================================
// HTTP File Transfer Operations
// ============================================================================

void MoonrakerFileTransferAPI::download_file(const std::string& root, const std::string& path,
                                             StringCallback on_success, ErrorCallback on_error) {
    on_success = helix::http_epoch::guard_reply(on_success, on_error, "download_file");
    // Validate inputs
    if (reject_invalid_path(path, "download_file", on_error))
        return;

    if (http_base_url_.empty()) {
        spdlog::error(
            "[Moonraker API] HTTP base URL not configured - call set_http_base_url first");
        report_connection_error(on_error, "download_file", "HTTP base URL not configured");
        return;
    }

    // Build URL: http://host:port/server/files/{root}/{path}
    // URL-encode the path to handle spaces and special characters
    std::string encoded_path = HUrl::escape(path, "/.-_");
    std::string url = http_base_url_ + "/server/files/" + root + "/" + encoded_path;

    // trace, not debug: the AD5X IFS backend polls Adventurer5M.json through
    // here every 5s (freshness backstop for zmod's silent on-printer edits), and
    // at debug these two lines dominate the 2000-line ring buffer — bundle
    // Z5V4K3NL's ring covered only ~9 minutes. Errors still log via
    // handle_http_response; callers log their own meaningful events.
    spdlog::trace("[Moonraker API] Downloading file: {}", url);

    // Run HTTP request in a tracked thread to ensure clean shutdown
    helix::http::HttpExecutor::slow().submit([url, path, on_success, on_error]() {
        auto resp = requests::get(url.c_str());

        if (!handle_http_response(resp, "download_file", on_error)) {
            return;
        }

        spdlog::trace("[Moonraker API] Downloaded {} bytes from {}", resp->body.size(), path);
        helix::MemoryMonitor::log_now("moonraker_download_done");

        if (on_success) {
            on_success(resp->body);
        }
    });
}

namespace {

// GETs @p len bytes from @p offset on the slow lane and hands them on. A
// Range-honouring 206 sends exactly those; a Range-ignoring 200 sends the
// whole file, whose first @p offset bytes are skipped as they stream past.
void submit_bounded_get(const std::string& url, const std::string& path, uint64_t offset,
                        size_t len, const char* op,
                        MoonrakerFileTransferAPI::StringCallback on_success,
                        MoonrakerFileTransferAPI::ErrorCallback on_error,
                        MoonrakerFileTransferAPI::CancelFlag cancelled) {
    helix::http::HttpExecutor::slow().submit([url, path, offset, len, op, on_success, on_error,
                                              cancelled]() {
        if (cancelled && cancelled->load()) {
            report_error(on_error, MoonrakerErrorType::UNKNOWN, op, "cancelled before it was sent");
            return;
        }
        auto req = std::make_shared<HttpRequest>();
        req->method = HTTP_GET;
        req->url = url;
        req->timeout = 30; // 30 second timeout

        // HTTP Range is inclusive: bytes=0-99 returns 100 bytes.
        req->SetHeader("Range",
                       "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + len - 1));

        // Stream the body through a per-chunk callback, the same pattern
        // requests::downloadFile uses. With http_cb set, libhv hands each body
        // chunk to us instead of accumulating resp->body, which is what lets
        // the transfer be stopped mid-body: the client's recv loop checks
        // req->cancel after every chunk, so cancelling the moment len bytes
        // have arrived closes the connection instead of letting a
        // Range-ignoring 200 push the whole file over the wire and occupy
        // this slow-lane worker for the full transfer.
        std::string body;
        uint64_t skip = 0;
        bool first_chunk = true;
        req->http_cb = [&req, &body, &skip, &first_chunk, offset,
                        len](HttpMessage* msg, http_parser_state state, const char* data,
                             size_t size) {
            if (state != HP_BODY || data == nullptr || size == 0) {
                return;
            }
            if (first_chunk) {
                first_chunk = false;
                // The client parses responses, so the message is one.
                skip = static_cast<HttpResponse*>(msg)->status_code == HTTP_STATUS_OK ? offset : 0;
            }
            if (skip >= size) {
                skip -= size;
                return;
            }
            data += skip;
            size -= static_cast<size_t>(skip);
            skip = 0;
            const size_t take = std::min(size, len - body.size());
            body.append(data, take);
            if (body.size() >= len) {
                req->Cancel();
            }
        };

        auto resp = requests::request(req);

        // Accept both 200 (full file) and 206 (partial content)
        if (!handle_http_response(resp, op, on_error, {200, 206})) {
            return;
        }

        spdlog::debug("[Moonraker API] Ranged download: {} bytes at {} from {} (status {})",
                      body.size(), offset, path, static_cast<int>(resp->status_code));

        if (resp->status_code == 200 && (offset > 0 || body.size() == len)) {
            spdlog::warn("[Moonraker API] Ranged download: server ignored Range for {} "
                         "(skipped {} bytes, aborted the transfer at {} more)",
                         path, offset, body.size());
        }

        if (on_success) {
            on_success(body);
        }
    });
}

// Validates the request and builds its URL; @p op names it in errors and logs.
void start_ranged_get(const std::string& http_base_url, const std::string& root,
                      const std::string& path, uint64_t offset, size_t len, const char* op,
                      MoonrakerFileTransferAPI::StringCallback on_success,
                      MoonrakerFileTransferAPI::ErrorCallback on_error,
                      MoonrakerFileTransferAPI::CancelFlag cancelled) {
    on_success = helix::http_epoch::guard_reply(on_success, on_error, op);
    if (reject_invalid_path(path, op, on_error))
        return;

    if (http_base_url.empty()) {
        spdlog::error(
            "[Moonraker API] HTTP base URL not configured - call set_http_base_url first");
        report_connection_error(on_error, op, "HTTP base URL not configured");
        return;
    }

    if (len == 0) {
        // An empty range is not expressible ("bytes=5-4" is invalid).
        report_connection_error(on_error, op, "length must be non-zero");
        return;
    }

    // Build URL: http://host:port/server/files/{root}/{path}
    std::string encoded_path = HUrl::escape(path, "/.-_");
    std::string url = http_base_url + "/server/files/" + root + "/" + encoded_path;

    spdlog::debug("[Moonraker API] {} ({} bytes at {}): {}", op, len, offset, url);
    submit_bounded_get(url, path, offset, len, op, std::move(on_success), std::move(on_error),
                       std::move(cancelled));
}

} // namespace

void MoonrakerFileTransferAPI::download_file_partial(const std::string& root,
                                                     const std::string& path, size_t max_bytes,
                                                     StringCallback on_success,
                                                     ErrorCallback on_error, CancelFlag cancelled) {
    start_ranged_get(http_base_url_, root, path, 0, max_bytes, "download_file_partial",
                     std::move(on_success), std::move(on_error), std::move(cancelled));
}

void MoonrakerFileTransferAPI::download_file_range(const std::string& root, const std::string& path,
                                                   uint64_t offset, size_t len,
                                                   StringCallback on_success,
                                                   ErrorCallback on_error, CancelFlag cancelled) {
    start_ranged_get(http_base_url_, root, path, offset, len, "download_file_range",
                     std::move(on_success), std::move(on_error), std::move(cancelled));
}

void MoonrakerFileTransferAPI::download_file_tail(const std::string& root, const std::string& path,
                                                  size_t max_bytes, StringCallback on_success,
                                                  ErrorCallback on_error) {
    on_success = helix::http_epoch::guard_reply(on_success, on_error, "download_file_tail");
    if (reject_invalid_path(path, "download_file_tail", on_error))
        return;

    if (http_base_url_.empty()) {
        spdlog::error(
            "[Moonraker API] HTTP base URL not configured - call set_http_base_url first");
        report_connection_error(on_error, "download_file_tail", "HTTP base URL not configured");
        return;
    }

    if (max_bytes == 0) {
        // A zero-length suffix range is not expressible ("bytes=-0" is invalid)
        // and asking for nothing is a caller bug, not a transfer to attempt.
        report_connection_error(on_error, "download_file_tail", "max_bytes must be non-zero");
        return;
    }

    std::string encoded_path = HUrl::escape(path, "/.-_");
    std::string url = http_base_url_ + "/server/files/" + root + "/" + encoded_path;

    spdlog::debug("[Moonraker API] Tail download (last {} bytes): {}", max_bytes, url);

    helix::http::HttpExecutor::slow().submit([url, path, max_bytes, on_success, on_error]() {
        auto req = std::make_shared<HttpRequest>();
        req->method = HTTP_GET;
        req->url = url;
        req->timeout = 30;

        // Suffix range: "bytes=-N" means the LAST N bytes, and is the only form
        // that does not require knowing the file length up front.
        req->SetHeader("Range", "bytes=-" + std::to_string(max_bytes));

        auto resp = requests::request(req);

        // 206 is the expected answer. 200 means the server ignored Range and
        // sent everything — wasteful but still correct content, so accept it
        // rather than failing a request whose data did arrive.
        if (!handle_http_response(resp, "download_file_tail", on_error, {200, 206})) {
            return;
        }

        spdlog::debug("[Moonraker API] Tail download: {} bytes from {} (status {})",
                      resp->body.size(), path, static_cast<int>(resp->status_code));

        // Same Range-ignoring guard as the head path, keeping the LAST
        // max_bytes: a tail reader parses the end of the file.
        if (resp->body.size() > max_bytes) {
            spdlog::warn("[Moonraker API] Tail download: server ignored Range for {} "
                         "(sent {} bytes, keeping last {})",
                         path, resp->body.size(), max_bytes);
            resp->body.erase(0, resp->body.size() - max_bytes);
        }

        if (on_success) {
            on_success(resp->body);
        }
    });
}

bool MoonrakerFileTransferAPI::supports_local_copies() const {
    return true;
}

bool MoonrakerFileTransferAPI::supports_tail_reads() const {
    return true;
}

bool MoonrakerFileTransferAPI::supports_range_reads() const {
    return true;
}

void MoonrakerFileTransferAPI::download_file_to_path(
    const std::string& root, const std::string& path, const std::string& dest_path,
    StringCallback on_success, ErrorCallback on_error, ProgressCallback on_progress) {
    on_success = helix::http_epoch::guard_reply(on_success, on_error, "download_file_to_path");
    if (http_base_url_.empty()) {
        spdlog::error("[Moonraker API] HTTP base URL not set - cannot download file");
        report_connection_error(on_error, "download_file_to_path", "HTTP base URL not configured");
        return;
    }

    // Build URL: http://host:port/server/files/{root}/{path}
    // URL-encode the path to handle spaces and special characters
    std::string encoded_path = HUrl::escape(path, "/.-_");
    std::string url = http_base_url_ + "/server/files/" + root + "/" + encoded_path;

    spdlog::debug("[Moonraker API] Streaming download: {} -> {}", url, dest_path);

    // Run HTTP request in a tracked thread to ensure clean shutdown
    // Use requests::downloadFile which streams directly to disk
    helix::http::HttpExecutor::slow().submit([url, path, dest_path, on_success, on_error,
                                              on_progress]() {
        // libhv's downloadFile progress callback signature matches our ProgressCallback
        size_t bytes_written = requests::downloadFile(url.c_str(), dest_path.c_str(), on_progress);

        if (bytes_written == 0) {
            spdlog::error("[Moonraker API] Streaming download failed: {} -> {}", url, dest_path);
            report_connection_error(on_error, "download_file_to_path",
                                    "Streaming download failed: " + path);
            return;
        }

        spdlog::info("[Moonraker API] Streamed {} bytes to {}", bytes_written, dest_path);

        if (on_success) {
            on_success(dest_path);
        }
    });
}

void MoonrakerFileTransferAPI::download_thumbnail(const std::string& thumbnail_path,
                                                  const std::string& cache_path,
                                                  StringCallback on_success,
                                                  ErrorCallback on_error) {
    on_success = helix::http_epoch::guard_reply(on_success, on_error, "download_thumbnail");
    // Validate inputs
    if (thumbnail_path.empty()) {
        spdlog::warn("[Moonraker API] Empty thumbnail path");
        report_error(on_error, MoonrakerErrorType::VALIDATION_ERROR, "download_thumbnail",
                     "Empty thumbnail path");
        return;
    }

    if (http_base_url_.empty()) {
        report_connection_error(on_error, "download_thumbnail", "HTTP base URL not configured");
        return;
    }

    // Build URL: http://host:port/server/files/gcodes/{thumbnail_path}
    // Thumbnail paths must be relative to the gcodes root (caller prepends subdir)
    // URL-encode the path to handle spaces and special characters
    // Leave /.-_ unescaped as they're valid in URL paths
    std::string encoded_path = HUrl::escape(thumbnail_path, "/.-_");
    std::string url = http_base_url_ + "/server/files/gcodes/" + encoded_path;

    spdlog::trace("[Moonraker API] Downloading thumbnail: {} -> {}", url, cache_path);

    // Thumbnails are small (tens of KB) and fetched in bursts when the file
    // browser scrolls. Run them on the fast lane so uploads/downloads on the
    // slow lane don't block the UI.
    helix::http::HttpExecutor::fast().submit([url, thumbnail_path, cache_path, on_success,
                                              on_error]() {
        auto resp = requests::get(url.c_str());

        if (!handle_http_response(resp, "download_thumbnail", on_error)) {
            return;
        }

        // The cache names this file .png and LVGL picks its decoder by that
        // name, so a JPEG thumbnail is re-encoded and anything else refused.
        const std::vector<uint8_t> png = helix::ensure_png({resp->body.begin(), resp->body.end()});
        if (png.empty()) {
            report_error(on_error, MoonrakerErrorType::VALIDATION_ERROR, "download_thumbnail",
                         "Not a PNG or decodable JPEG thumbnail: " + thumbnail_path);
            return;
        }

        // Replace, never rewrite: a reader of the cached file sees the old
        // image or the new one, not a partial download.
        if (!helix::text_io::write_file_atomic(
                cache_path, {reinterpret_cast<const char*>(png.data()), png.size()})) {
            spdlog::error("[Moonraker API] Failed to write cache file: {}", cache_path);
            report_error(on_error, MoonrakerErrorType::UNKNOWN, "download_thumbnail",
                         "Failed to write cache file: " + cache_path);
            return;
        }

        spdlog::trace("[Moonraker API] Cached thumbnail {} bytes -> {}", png.size(), cache_path);
        helix::MemoryMonitor::log_now("moonraker_thumb_downloaded");

        if (on_success) {
            on_success(cache_path);
        }
    });
}

void MoonrakerFileTransferAPI::upload_file(const std::string& root, const std::string& path,
                                           const std::string& content, SuccessCallback on_success,
                                           ErrorCallback on_error) {
    upload_file_with_name(root, path, path, content, on_success, on_error);
}

void MoonrakerFileTransferAPI::upload_file_with_name(const std::string& root,
                                                     const std::string& path,
                                                     const std::string& filename,
                                                     const std::string& content,
                                                     SuccessCallback on_success,
                                                     ErrorCallback on_error, UploadLane lane) {
    // `path` only supplies the destination DIRECTORY here - the name written on
    // the printer comes from the separate `filename` parameter, and the form
    // field below is only set when `path` actually contains a directory
    // component. An empty `path` therefore means "the root of `root`", which is
    // a legitimate destination: Moonraker treats an omitted `path` form field as
    // the root of the selected root. Guarding with `!path.empty()` is the same
    // empty-means-root convention list_files() and get_directory() already use
    // in moonraker_file_api.cpp. is_safe_path("") stays false - an empty name is
    // never a valid file, which is what every other caller is asking about.
    if (!path.empty() && reject_invalid_path(path, "upload_file", on_error))
        return;

    // The filename is the part that reaches the multipart form verbatim, and it
    // is never legitimately empty. Validating it here is what keeps
    // upload_file(root, "", content) - which forwards path as the filename -
    // rejected now that an empty `path` no longer is.
    if (reject_invalid_path(filename, "upload_file", on_error))
        return;

    if (http_base_url_.empty()) {
        spdlog::error(
            "[Moonraker API] HTTP base URL not configured - call set_http_base_url first");
        report_connection_error(on_error, "upload_file", "HTTP base URL not configured");
        return;
    }

    // Build URL: http://host:port/server/files/upload
    std::string url = http_base_url_ + "/server/files/upload";

    spdlog::debug("[Moonraker API] Uploading {} bytes to {}/{}", content.size(), root, path);

    // A small buffer (config edits, PRINT_START shim, macro files) takes the
    // fast lane so it does not queue behind large gcode uploads on the slow
    // one; a buffer of megabytes (a camera photo) takes the slow lane so it
    // does not hold up small REST work. upload_file_from_path streams large
    // files from disk.
    auto& executor = lane == UploadLane::Bulk ? helix::http::HttpExecutor::slow()
                                              : helix::http::HttpExecutor::fast();
    executor.submit([url, root, path, filename, content, on_success, on_error]() {
        // Create multipart form request
        auto req = std::make_shared<HttpRequest>();
        req->method = HTTP_POST;
        req->url = url;
        req->timeout = 120; // 2 minute timeout for uploads
        req->content_type = MULTIPART_FORM_DATA;

        // Add root parameter (e.g., "gcodes" or "config")
        req->SetFormData("root", root);

        // Add path parameter if uploading to subdirectory
        if (path.find('/') != std::string::npos) {
            req->SetFormData("path", helix::gcode_dir_of(path));
        }

        // Add file content with filename
        // Use hv::FormData for multipart file upload
        hv::FormData file_data;
        file_data.content = content;
        file_data.filename = filename;
        req->form["file"] = file_data;
        helix::MemoryMonitor::log_now("moonraker_upload_start");

        // Send request
        auto resp = requests::request(req);

        // Upload accepts 200 or 201
        if (!handle_http_response(resp, "upload_file", on_error, {200, 201})) {
            return;
        }

        spdlog::info("[Moonraker API] Successfully uploaded {} ({} bytes)", path, content.size());

        if (on_success) {
            on_success();
        }
    });
}

void MoonrakerFileTransferAPI::upload_file_from_path(
    const std::string& root, const std::string& dest_path, const std::string& local_path,
    SuccessCallback on_success, ErrorCallback on_error, ProgressCallback on_progress) {
    // Validate inputs
    if (reject_invalid_path(dest_path, "upload_file_from_path", on_error))
        return;

    if (http_base_url_.empty()) {
        spdlog::error(
            "[Moonraker API] HTTP base URL not configured - call set_http_base_url first");
        report_connection_error(on_error, "upload_file_from_path", "HTTP base URL not configured");
        return;
    }

    // Get file size for logging
    std::error_code ec;
    auto file_size = std::filesystem::file_size(local_path, ec);
    if (ec) {
        spdlog::error("[Moonraker API] Failed to get file size for {}: {}", local_path,
                      ec.message());
        report_error(on_error, MoonrakerErrorType::FILE_NOT_FOUND, "upload_file_from_path",
                     "Failed to get file size: " + local_path);
        return;
    }

    // Extract filename from dest_path (may differ from local_path basename)
    std::string filename = helix::gcode::get_filename_basename(dest_path);

    // The directory part of dest_path, empty for a bare name
    std::string directory = helix::gcode_dir_of(dest_path);

    std::string url = http_base_url_ + "/server/files/upload";

    spdlog::info("[Moonraker API] Streaming upload {} ({} bytes) to {}/{}", local_path, file_size,
                 root, dest_path);

    // Build form params for Moonraker (root, and optionally path for subdirectory)
    std::map<std::string, std::string> params;
    params["root"] = root;
    if (!directory.empty()) {
        params["path"] = directory;
    }

    // Run streaming upload in a tracked thread using libhv's uploadLargeFormFile
    helix::http::HttpExecutor::slow().submit(
        [url, params, filename, local_path, file_size, on_success, on_error, on_progress]() {
            // Use libhv's streaming multipart upload with custom filename
            // Combine external progress callback with internal logging
            size_t last_progress_log = 0;
            auto progress_cb = [&last_progress_log, on_progress](size_t sent, size_t total) {
                // Internal logging every 10MB
                if (sent - last_progress_log >= 10 * 1024 * 1024) {
                    spdlog::debug("[Moonraker API] Upload progress: {}/{} bytes ({:.1f}%)", sent,
                                  total, 100.0 * sent / total);
                    last_progress_log = sent;
                }
                // External progress callback
                if (on_progress) {
                    on_progress(sent, total);
                }
            };

            // Need non-const copy for libhv API
            auto params_copy = params;

            auto resp = requests::uploadLargeFormFile(url.c_str(), "file", local_path.c_str(),
                                                      filename.c_str(), params_copy, progress_cb);

            // Upload accepts 200 or 201
            if (!handle_http_response(resp, "upload_file_from_path", on_error, {200, 201})) {
                return;
            }

            spdlog::info("[Moonraker API] Streaming upload complete: {} ({} bytes)", filename,
                         file_size);
            helix::MemoryMonitor::log_now("moonraker_upload_streaming_complete");

            if (on_success) {
                on_success();
            }
        });
}
