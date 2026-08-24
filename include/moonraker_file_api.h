// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_file_api.h
 * @brief File management operations via Moonraker (WebSocket-based)
 *
 * Extracted from MoonrakerAPI to encapsulate all WebSocket-based file management
 * functionality in a dedicated class. Uses MoonrakerClient for JSON-RPC transport.
 *
 * Covers: list, get directory, get metadata, metascan, delete, move, copy,
 * create/delete directory. Does NOT include HTTP file transfers (download,
 * upload, thumbnail) which remain in MoonrakerAPI.
 */

#pragma once

#include "i_moonraker_sub_apis.h"
#include "moonraker_error.h"
#include "moonraker_types.h"

#include <functional>
#include <string>
#include <vector>

namespace helix {

// Forward declaration
class IMoonrakerClient;

/**
 * @brief Parse a server.files.roots response into FileRoot entries
 *
 * Accepts both the JSON-RPC envelope (`{"result": [...]}`) and a bare array.
 * Entries missing a usable name or path are dropped rather than half-filled:
 * a root with no path cannot be compared against anything, and one with no
 * name cannot be asked for.
 */
std::vector<FileRoot> parse_file_roots(const json& response);

} // namespace helix

/**
 * @brief Final path component of a Moonraker directory/file name.
 *
 * Stock Moonraker returns bare leaf names in the `dirname`/`filename` fields of
 * server.files.get_directory (e.g. "Gridfinity"). FlashForge's Moonraker fork
 * (Adventurer 5X) instead returns paths relative to the gcodes root
 * (e.g. "Feinkost/Gridfinity"). Stored verbatim, those got concatenated onto the
 * already-current path by the Print Files navigator, producing a doubled segment
 * ("Feinkost/Feinkost/Gridfinity") that the server reports as missing and the
 * panel then retried forever (debug bundle TJVQDCZ6).
 *
 * Normalizing to the leaf lets navigation append a single segment correctly.
 * It is a no-op for stock leaf names (leaf in -> leaf out), and tolerates a
 * trailing slash, so no other printer is affected.
 */
inline std::string moonraker_path_leaf(const std::string& name) {
    size_t end = name.find_last_not_of('/');
    if (end == std::string::npos) {
        return std::string(); // empty, or all slashes
    }
    size_t slash = name.find_last_of('/', end);
    size_t start = (slash == std::string::npos) ? 0 : slash + 1;
    return name.substr(start, end - start + 1);
}

/**
 * @brief File Management API operations via Moonraker
 *
 * Provides high-level operations for listing, querying, and managing files
 * through Moonraker's server.files.* WebSocket endpoints. All methods are
 * asynchronous with callbacks.
 *
 * Usage:
 *   MoonrakerFileAPI files(client);
 *   files.list_files("gcodes", "", true,
 *       [](const auto& files) { ... },
 *       [](const auto& err) { ... });
 */
class MoonrakerFileAPI : public IFilesAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using FileListCallback = std::function<void(const std::vector<FileInfo>&)>;
    using FileMetadataCallback = std::function<void(const FileMetadata&)>;
    using FileRootsCallback = std::function<void(const std::vector<FileRoot>&)>;

    /**
     * @brief Constructor
     *
     * @param client MoonrakerClient instance (must remain valid during API lifetime)
     */
    explicit MoonrakerFileAPI(helix::IMoonrakerClient& client);
    virtual ~MoonrakerFileAPI() = default;

    // ========================================================================
    // File Management Operations
    // ========================================================================

    /**
     * @brief List the file manager's roots (server.files.roots)
     *
     * @param on_success Callback with the parsed roots, each carrying its absolute
     *                   on-disk path and permissions
     * @param on_error Error callback
     */
    void get_file_roots(FileRootsCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief List files in a directory
     *
     * @param root Root directory ("gcodes", "config", "timelapse")
     * @param path Subdirectory path (empty for root)
     * @param recursive Include subdirectories
     * @param on_success Callback with file list
     * @param on_error Error callback
     */
    void list_files(const std::string& root, const std::string& path, bool recursive,
                    FileListCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Get directory contents with explicit directory entries
     *
     * Unlike list_files() which returns a flat list, this method returns
     * both files AND directories in the specified path. This is needed for
     * proper directory navigation in the file browser.
     *
     * Uses server.files.get_directory endpoint which returns:
     * - dirs: Array of {dirname, modified, size, permissions}
     * - files: Array of {filename, modified, size, permissions}
     *
     * @param root Root directory ("gcodes", "config", "timelapse")
     * @param path Subdirectory path (empty for root)
     * @param on_success Callback with file list (directories have is_dir=true)
     * @param on_error Error callback
     *
     * @note virtual so tests can inject a capturing sub-API that defers/records
     *       the callbacks (the class is already polymorphic via its virtual
     *       dtor, so this adds no new ABI/vtable cost).
     */
    void get_directory(const std::string& root, const std::string& path,
                       FileListCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Get detailed metadata for a file
     *
     * @param filename Full path to file (relative to gcodes root)
     * @param on_success Callback with metadata
     * @param on_error Error callback
     * @param silent If true, don't emit RPC_ERROR events (no toast on failure)
     *
     * @note virtual so tests can inject a capturing sub-API. The class is already
     *       polymorphic (virtual dtor), so this adds no new ABI/vtable cost.
     */
    void get_file_metadata(const std::string& filename, FileMetadataCallback on_success,
                           ErrorCallback on_error, bool silent = false) override;

    /**
     * @brief Trigger metadata scan for a file
     *
     * Forces Moonraker to parse and index a file's metadata. Useful when
     * get_file_metadata returns 404 (file exists but not indexed).
     * Returns the parsed metadata on success.
     *
     * @param filename Full path to file (relative to gcodes root)
     * @param on_success Callback with metadata
     * @param on_error Error callback
     * @param silent If true, don't emit RPC_ERROR events (no toast on failure)
     */
    void metascan_file(const std::string& filename, FileMetadataCallback on_success,
                       ErrorCallback on_error, bool silent = true) override;

    /**
     * @brief Delete a file
     *
     * @param filename Full path to file (relative to gcodes root)
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void delete_file(const std::string& filename, SuccessCallback on_success,
                     ErrorCallback on_error) override;

    /**
     * @brief Move or rename a file
     *
     * @param source Source path (e.g., "gcodes/old_dir/file.gcode")
     * @param dest Destination path (e.g., "gcodes/new_dir/file.gcode")
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void move_file(const std::string& source, const std::string& dest, SuccessCallback on_success,
                   ErrorCallback on_error) override;

    /**
     * @brief Copy a file
     *
     * @param source Source path (e.g., "gcodes/original.gcode")
     * @param dest Destination path (e.g., "gcodes/copy.gcode")
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void copy_file(const std::string& source, const std::string& dest, SuccessCallback on_success,
                   ErrorCallback on_error) override;

    /**
     * @brief Create a directory
     *
     * @param path Directory path (e.g., "gcodes/my_folder")
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void create_directory(const std::string& path, SuccessCallback on_success,
                          ErrorCallback on_error) override;

    /**
     * @brief Delete a directory
     *
     * @param path Directory path (e.g., "gcodes/old_folder")
     * @param force Force deletion even if not empty
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void delete_directory(const std::string& path, bool force, SuccessCallback on_success,
                          ErrorCallback on_error) override;

  protected:
    helix::IMoonrakerClient& client_;

    /**
     * @brief Parse file list response from server.files.list
     */
    std::vector<FileInfo> parse_file_list(const json& response);

    /**
     * @brief Parse metadata response from server.files.metadata
     */
    FileMetadata parse_file_metadata(const json& response);
};
