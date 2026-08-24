// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_file_transfer_api.h
 * @brief HTTP file transfer operations via Moonraker
 *
 * Extracted from MoonrakerAPI to encapsulate all HTTP file transfer functionality
 * (downloads, uploads, thumbnails) in a dedicated class. Owns an HTTP thread pool
 * for async file transfer operations.
 */

#pragma once

#include "i_moonraker_sub_apis.h"
#include "moonraker_error.h"

#include <functional>
#include <string>

// Forward declarations
namespace helix {
class IMoonrakerClient;
} // namespace helix

/**
 * @brief HTTP File Transfer API operations via Moonraker
 *
 * Provides HTTP-based file download and upload operations through Moonraker's
 * /server/files/ endpoints. Manages its own thread pool for async HTTP requests
 * with proper lifecycle management (thread joining on destruction).
 *
 * Thread safety: All file transfer methods launch background HTTP threads.
 * Callbacks are invoked from those threads. Callers must ensure their callback
 * captures remain valid for the duration of the request.
 *
 * Usage:
 *   MoonrakerFileTransferAPI transfers(client, http_base_url);
 *   transfers.download_file("gcodes", "test.gcode",
 *       [](const std::string& content) { ... },
 *       [](const auto& err) { ... });
 */
class MoonrakerFileTransferAPI : public ITransfersAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using StringCallback = std::function<void(const std::string&)>;

    /**
     * @brief Progress callback for file transfer operations
     *
     * Called periodically during download/upload with bytes transferred and total.
     * NOTE: Called from background HTTP thread - use helix::ui::async_call() for UI updates.
     *
     * @param current Bytes transferred so far
     * @param total Total bytes to transfer
     */
    using ProgressCallback = std::function<void(size_t current, size_t total)>;

    /**
     * @brief Constructor
     *
     * @param client MoonrakerClient instance (must remain valid during API lifetime)
     * @param http_base_url Reference to HTTP base URL string (owned by MoonrakerAPI)
     */
    MoonrakerFileTransferAPI(helix::IMoonrakerClient& client, const std::string& http_base_url);

    /**
     * @brief Destructor — joins all pending HTTP threads
     *
     * Signals shutdown and waits for active HTTP threads with timeout.
     * Threads that don't complete within 2 seconds are detached.
     */
    virtual ~MoonrakerFileTransferAPI();

    // ========================================================================
    // Download Operations
    // ========================================================================

    /**
     * @brief Download a file's content from the printer via HTTP
     *
     * Uses GET request to /server/files/{root}/{path} endpoint.
     * The file content is returned as a string in the callback.
     *
     * Virtual to allow mocking in tests (MoonrakerAPIMock reads local files).
     *
     * @param root Root directory ("gcodes", "config", etc.)
     * @param path File path relative to root
     * @param on_success Callback with file content as string
     * @param on_error Error callback
     */
    void download_file(const std::string& root, const std::string& path, StringCallback on_success,
                       ErrorCallback on_error) override;

    /**
     * @brief Download only the first N bytes of a file (for scanning preambles)
     *
     * Uses HTTP Range request to fetch only the beginning of a file.
     * Ideal for scanning G-code files where operations are in the preamble.
     *
     * @param root Root directory ("gcodes", "config", etc.)
     * @param path File path relative to root
     * @param max_bytes Maximum bytes to download (default 100KB)
     * @param on_success Callback with partial file content as string
     * @param on_error Error callback
     */
    /**
     * @brief Download only the last N bytes of a file (for scanning footers)
     *
     * HTTP suffix range (`bytes=-N`). Slicers that emit their settings block
     * after the print body — OrcaSlicer's `filament_colour` among them — put the
     * data a preamble scan can never reach.
     *
     * @param root Root directory ("gcodes", "config", etc.)
     * @param path File path relative to root
     * @param max_bytes Maximum bytes to download from the end
     * @param on_success Callback with the trailing file content as string
     * @param on_error Error callback
     */
    void download_file_tail(const std::string& root, const std::string& path, size_t max_bytes,
                            StringCallback on_success, ErrorCallback on_error) override;

    void download_file_partial(const std::string& root, const std::string& path, size_t max_bytes,
                               StringCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Download a file directly to disk (streaming, low memory)
     *
     * Unlike download_file() which loads entire content into memory,
     * this streams chunks directly to disk as they arrive. Essential
     * for large G-code files on memory-constrained devices like AD5M.
     *
     * Virtual to allow mocking in tests.
     *
     * @param root Root directory ("gcodes", "config", etc.)
     * @param path File path relative to root
     * @param dest_path Local filesystem path to write to
     * @param on_success Callback with dest_path on success
     * @param on_error Error callback
     * @param on_progress Optional callback for progress updates (called from HTTP thread)
     */
    void download_file_to_path(const std::string& root, const std::string& path,
                               const std::string& dest_path, StringCallback on_success,
                               ErrorCallback on_error,
                               ProgressCallback on_progress = nullptr) override;

    /**
     * @brief Download a thumbnail image and cache it locally
     *
     * Downloads thumbnail from Moonraker's HTTP server and saves to a local cache file.
     * The callback receives the local file path (suitable for LVGL image loading).
     *
     * Virtual to allow mocking in tests.
     *
     * @param thumbnail_path Relative path from metadata (e.g., ".thumbnails/file.png")
     * @param cache_path Local filesystem path to save the thumbnail
     * @param on_success Callback with local cache path
     * @param on_error Error callback
     */
    void download_thumbnail(const std::string& thumbnail_path, const std::string& cache_path,
                            StringCallback on_success, ErrorCallback on_error) override;

    // ========================================================================
    // Upload Operations
    // ========================================================================

    /**
     * @brief Upload file content to the printer via HTTP multipart form
     *
     * Uses POST request to /server/files/upload endpoint with multipart form data.
     * Suitable for G-code files, config files, and macro files.
     *
     * Virtual to allow mocking in tests (MoonrakerAPIMock logs but doesn't write).
     *
     * @param root Root directory ("gcodes", "config", etc.)
     * @param path Destination path relative to root
     * @param content File content to upload
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void upload_file(const std::string& root, const std::string& path, const std::string& content,
                     SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Upload file content with custom filename
     *
     * Like upload_file() but allows specifying a different filename for the
     * multipart form than the path. Useful when uploading to a subdirectory.
     *
     * Virtual to allow mocking in tests (MoonrakerAPIMock logs but doesn't write).
     *
     * @param root Root directory ("gcodes", "config", etc.)
     * @param path Destination path relative to root (e.g., ".helix_temp/foo.gcode").
     *             Only its directory component is used; pass "" to upload straight
     *             to the root of @p root (same empty-means-root convention as
     *             list_files()/get_directory()).
     * @param filename Filename for form (e.g., ".helix_temp/foo.gcode"). Must be
     *                 non-empty and traversal-free - it is used verbatim.
     * @param content File content to upload
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void upload_file_with_name(const std::string& root, const std::string& path,
                               const std::string& filename, const std::string& content,
                               SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Upload file from local filesystem path (streaming, low memory)
     *
     * Streams file from disk to Moonraker in chunks, never loading the entire
     * file into memory. Essential for large G-code files on memory-constrained
     * devices like AD5M.
     *
     * Virtual to allow mocking in tests.
     *
     * @param root Root directory ("gcodes", "config", etc.)
     * @param dest_path Destination path relative to root (e.g., ".helix_temp/foo.gcode")
     * @param local_path Local filesystem path to read from
     * @param on_success Success callback
     * @param on_error Error callback
     * @param on_progress Optional callback for progress updates (called from HTTP thread)
     */
    void upload_file_from_path(const std::string& root, const std::string& dest_path,
                               const std::string& local_path, SuccessCallback on_success,
                               ErrorCallback on_error,
                               ProgressCallback on_progress = nullptr) override;

  protected:
    helix::IMoonrakerClient& client_;
    const std::string& http_base_url_;
};
