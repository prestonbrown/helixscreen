// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_file_transfer_api.h"

#include "ui_error_reporting.h"
#include "ui_notification.h"

#include "http_executor.h"
#include "hv/hfile.h"
#include "hv/hurl.h"
#include "hv/requests.h"
#include "memory_monitor.h"
#include "moonraker_api_internal.h"
#include "spdlog/spdlog.h"

#include <filesystem>
#include <fstream>
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

void MoonrakerFileTransferAPI::download_file_partial(const std::string& root,
                                                     const std::string& path, size_t max_bytes,
                                                     StringCallback on_success,
                                                     ErrorCallback on_error) {
    // Validate inputs
    if (reject_invalid_path(path, "download_file_partial", on_error))
        return;

    if (http_base_url_.empty()) {
        spdlog::error(
            "[Moonraker API] HTTP base URL not configured - call set_http_base_url first");
        report_connection_error(on_error, "download_file_partial", "HTTP base URL not configured");
        return;
    }

    // Build URL: http://host:port/server/files/{root}/{path}
    std::string encoded_path = HUrl::escape(path, "/.-_");
    std::string url = http_base_url_ + "/server/files/" + root + "/" + encoded_path;

    spdlog::debug("[Moonraker API] Partial download (first {} bytes): {}", max_bytes, url);

    // Run HTTP request in a tracked thread
    helix::http::HttpExecutor::slow().submit([url, path, max_bytes, on_success, on_error]() {
        // Create request with Range header for partial content
        auto req = std::make_shared<HttpRequest>();
        req->method = HTTP_GET;
        req->url = url;
        req->timeout = 30; // 30 second timeout

        // HTTP Range header: bytes=0-{max_bytes-1}
        // Note: Range is inclusive, so bytes=0-99 returns 100 bytes
        std::string range_header = "bytes=0-" + std::to_string(max_bytes - 1);
        req->SetHeader("Range", range_header);

        auto resp = requests::request(req);

        // Accept both 200 (full file) and 206 (partial content)
        if (!handle_http_response(resp, "download_file_partial", on_error, {200, 206})) {
            return;
        }

        spdlog::debug("[Moonraker API] Partial download: {} bytes from {} (status {})",
                      resp->body.size(), path, static_cast<int>(resp->status_code));

        if (on_success) {
            on_success(resp->body);
        }
    });
}

void MoonrakerFileTransferAPI::download_file_tail(const std::string& root, const std::string& path,
                                                  size_t max_bytes, StringCallback on_success,
                                                  ErrorCallback on_error) {
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

        if (on_success) {
            on_success(resp->body);
        }
    });
}

void MoonrakerFileTransferAPI::download_file_to_path(
    const std::string& root, const std::string& path, const std::string& dest_path,
    StringCallback on_success, ErrorCallback on_error, ProgressCallback on_progress) {
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
    helix::http::HttpExecutor::fast().submit(
        [url, thumbnail_path, cache_path, on_success, on_error]() {
            auto resp = requests::get(url.c_str());

            if (!handle_http_response(resp, "download_thumbnail", on_error)) {
                return;
            }

            // Write to cache file
            std::ofstream file(cache_path, std::ios::binary);
            if (!file) {
                spdlog::error("[Moonraker API] Failed to create cache file: {}", cache_path);
                report_error(on_error, MoonrakerErrorType::UNKNOWN, "download_thumbnail",
                             "Failed to create cache file: " + cache_path);
                return;
            }

            file.write(resp->body.data(), static_cast<std::streamsize>(resp->body.size()));
            file.close();

            spdlog::trace("[Moonraker API] Cached thumbnail {} bytes -> {}", resp->body.size(),
                          cache_path);
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

void MoonrakerFileTransferAPI::upload_file_with_name(
    const std::string& root, const std::string& path, const std::string& filename,
    const std::string& content, SuccessCallback on_success, ErrorCallback on_error) {
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

    // Memory-buffer uploads are small (config edits, PRINT_START shim,
    // macro files). Fast lane so they don't queue behind large gcode
    // uploads on the slow lane. Use upload_file_from_path for large
    // streaming uploads.
    helix::http::HttpExecutor::fast().submit([url, root, path, filename, content, on_success,
                                              on_error]() {
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
            // Extract directory from path
            size_t last_slash = path.rfind('/');
            if (last_slash != std::string::npos) {
                std::string directory = path.substr(0, last_slash);
                req->SetFormData("path", directory);
            }
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
    std::string filename = dest_path;
    size_t last_slash = dest_path.rfind('/');
    if (last_slash != std::string::npos) {
        filename = dest_path.substr(last_slash + 1);
    }

    // Extract directory from path (if any)
    std::string directory;
    if (last_slash != std::string::npos) {
        directory = dest_path.substr(0, last_slash);
    }

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
