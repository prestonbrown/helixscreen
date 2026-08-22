// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_parser.h"
#include "moonraker_client_mock_internal.h"
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>

// Directory path for thumbnail cache (test G-code dir comes from RuntimeConfig::TEST_GCODE_DIR)
static constexpr const char* THUMBNAIL_CACHE_DIR = "build/thumbnail_cache";

// Alias for cleaner code - use shared constant from RuntimeConfig
#define TEST_GCODE_DIR RuntimeConfig::TEST_GCODE_DIR

/**
 * @brief Scan test directory for G-code files
 * @return Vector of filenames (not full paths)
 */
static std::vector<std::string> scan_mock_gcode_files() {
    std::vector<std::string> files;

    DIR* dir = opendir(TEST_GCODE_DIR);
    if (!dir) {
        spdlog::warn("[MoonrakerClientMock] Cannot open test G-code directory: {}", TEST_GCODE_DIR);
        return files;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;

        // Skip hidden files and non-gcode files
        if (name[0] == '.' || name.length() < 7) {
            continue;
        }

        // Check for .gcode extension (case insensitive)
        std::string ext = name.substr(name.length() - 6);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".gcode") {
            continue;
        }

        files.push_back(name);
    }

    closedir(dir);
    std::sort(files.begin(), files.end());

    spdlog::debug("[MoonrakerClientMock] Found {} mock G-code files", files.size());
    return files;
}

/**
 * @brief Build mock JSON response for server.files.list
 * @param path Directory path relative to gcodes root (empty = root)
 * @return JSON response matching real Moonraker format (flat array in result)
 *
 * Real Moonraker server.files.list returns:
 *   {"result": [{"path": "file.gcode", "modified": 123.0, "size": 456, "permissions": "rw"}, ...]}
 *
 * Note: Directories are NOT included in server.files.list - they come from
 * server.files.get_directory
 */
// Flag to simulate USB symlink presence (for testing)
static bool g_mock_usb_symlink_active = false;

void mock_set_usb_symlink_active(bool active) {
    g_mock_usb_symlink_active = active;
    spdlog::debug("[MoonrakerClientMock] USB symlink simulation: {}",
                  active ? "active" : "inactive");
}

static json build_mock_file_list_response(const std::string& root, const std::string& path = "") {
    json result_array = json::array();

    // Mock timelapse video files
    if (root == "timelapse" || path == "timelapse") {
        // Simulate a set of timelapse recordings with varied sizes and dates
        result_array.push_back({{"path", "benchy_timelapse_20260310.mp4"},
                                {"size", 52428800},           // 50 MB
                                {"modified", 1773158400.0}}); // 2026-03-10
        result_array.push_back({{"path", "vase_spiral_20260308.mp4"},
                                {"size", 128849018},          // ~123 MB
                                {"modified", 1772985600.0}}); // 2026-03-08
        result_array.push_back({{"path", "calibration_cube_20260305.mp4"},
                                {"size", 15728640},           // 15 MB
                                {"modified", 1772730000.0}}); // 2026-03-05
        result_array.push_back({{"path", "articulated_dragon_20260301.mp4"},
                                {"size", 314572800},          // 300 MB
                                {"modified", 1772384400.0}}); // 2026-03-01
        result_array.push_back({{"path", "flexi_rex_20260225.mkv"},
                                {"size", 89128960},           // 85 MB
                                {"modified", 1772038800.0}}); // 2026-02-25
        // Companion thumbnail (should be filtered out by is_video_file)
        result_array.push_back({{"path", "benchy_timelapse_20260310.thumb.jpg"},
                                {"size", 24576},
                                {"modified", 1773158400.0}});

        json response = {{"result", result_array}};
        spdlog::debug("[MoonrakerClientMock] Returning {} mock timelapse files",
                      result_array.size());
        return response;
    }

    // Simulate USB symlink directory
    if (path == "usb" && g_mock_usb_symlink_active) {
        // Return fake USB files to simulate symlink present
        result_array.push_back(
            {{"path", "usb/test_usb_file.gcode"}, {"size", 12345}, {"modified", 1700000000.0}});
        json response = {{"result", result_array}};
        spdlog::debug("[MoonrakerClientMock] Simulating USB symlink with {} files",
                      result_array.size());
        return response;
    }

    // Mock accelerometer CSV data files for belt tension / input shaper calibration
    if (root == "config" && path == "data_store") {
        result_array.push_back({{"path", "raw_data_belt_path_a-20260310_120000.csv"},
                                {"size", 2048},
                                {"modified", 1773158400.0}});
        result_array.push_back({{"path", "raw_data_belt_path_b-20260310_120001.csv"},
                                {"size", 2048},
                                {"modified", 1773158401.0}});
        result_array.push_back({{"path", "raw_data_x-20260310_115000.csv"},
                                {"size", 4096},
                                {"modified", 1773154800.0}});
        result_array.push_back({{"path", "raw_data_y-20260310_115001.csv"},
                                {"size", 4096},
                                {"modified", 1773154801.0}});

        json response = {{"result", result_array}};
        spdlog::debug("[MoonrakerClientMock] Returning {} mock data_store files",
                      result_array.size());
        return response;
    }

    if (path.empty() || path == "gcodes" || path == "gcodes/") {
        // Root directory - scan real files from test gcode directory
        auto filenames = scan_mock_gcode_files();

        for (const auto& filename : filenames) {
            std::string full_path = std::string(TEST_GCODE_DIR) + "/" + filename;

            struct stat file_stat;
            uint64_t size = 0;
            double modified = 0.0;
            if (stat(full_path.c_str(), &file_stat) == 0) {
                size = static_cast<uint64_t>(file_stat.st_size);
                modified = static_cast<double>(file_stat.st_mtime);
            }

            // Real Moonraker format: flat array with "path" key (not "filename")
            json file_entry = {
                {"path", filename}, {"size", size}, {"modified", modified}, {"permissions", "rw"}};
            result_array.push_back(file_entry);
        }

        // Note: We only return real files from TEST_GCODE_DIR
        // Fake subdirectory entries were removed to prevent thumbnail extraction warnings
    }
    // Unknown paths return empty lists

    json response = {{"result", result_array}};

    spdlog::debug("[MoonrakerClientMock] Built mock file list for path '{}': {} files",
                  path.empty() ? "/" : path, result_array.size());
    return response;
}

/**
 * @brief Build mock JSON response for server.files.metadata
 * @param filename Filename to get metadata for
 * @return JSON response matching Moonraker format
 */
static json build_mock_file_metadata_response(const std::string& filename) {
    // Handle case where filename already includes the test directory prefix
    // (happens when CLI passes --gcode-file with full path)
    std::string clean_filename = filename;
    std::string prefix = std::string(TEST_GCODE_DIR) + "/";
    if (filename.find(prefix) == 0) {
        clean_filename = filename.substr(prefix.length());
    }
    std::string full_path = std::string(TEST_GCODE_DIR) + "/" + clean_filename;

    // Get file info from filesystem
    struct stat file_stat;
    uint64_t size = 0;
    double modified = 0.0;
    if (stat(full_path.c_str(), &file_stat) == 0) {
        size = static_cast<uint64_t>(file_stat.st_size);
        modified = static_cast<double>(file_stat.st_mtime);
    }

    // Extract metadata from G-code header
    auto header_meta = helix::gcode::extract_header_metadata(full_path);

    // Get cached thumbnail path (creates cache if needed)
    std::string thumbnail_path = helix::gcode::get_cached_thumbnail(full_path, THUMBNAIL_CACHE_DIR);

    // HELIX_MOCK_REMOTE_THUMBS=1 — advertise a Moonraker-relative path instead
    // of the local cache file. PrintSelectPanel branches on whether the
    // advertised path exists on disk, so a local path (the default) short-cuts
    // straight to the prescaler and the HTTP download is never exercised. That
    // is why --test could not reach the cold-fetch pipeline that bundle
    // 6F3QJLFG implicates (#960). MockHttpFileServer serves these.
    static const bool remote_thumbs = [] {
        const char* v = std::getenv("HELIX_MOCK_REMOTE_THUMBS");
        return v && v[0] && std::string(v) != "0";
    }();
    if (remote_thumbs && !thumbnail_path.empty()) {
        std::string base = filename;
        const size_t dot = base.rfind('.');
        if (dot != std::string::npos) {
            base = base.substr(0, dot);
        }
        thumbnail_path = ".thumbs/" + base + "-300x300.png";
    }

    json thumbnails = json::array();
    if (!thumbnail_path.empty()) {
        // Return relative path to cached thumbnail (no LVGL prefix - that's a UI concern)
        // Format must match Moonraker's response structure: array of objects with dimensions
        thumbnails.push_back({{"relative_path", thumbnail_path},
                              {"width", 300},
                              {"height", 300},
                              {"size", 16384}}); // approximate file size in bytes
    }

    // Convert tool colors vector to JSON array
    json filament_colors = json::array();
    for (const auto& color : header_meta.tool_colors) {
        filament_colors.push_back(color);
    }

    // Per-tool filament weights — drives "unused tool" filtering in the
    // material-mismatch check (only tools with >0 weight actually extrude).
    json filament_weights = json::array();
    for (double g : header_meta.filament_used_per_tool_g) {
        filament_weights.push_back(g);
    }

    // Use fallback values for mock when G-code headers lack metadata
    double estimated_time =
        (header_meta.estimated_time_seconds > 0) ? header_meta.estimated_time_seconds : 300.0;
    double filament_mm = (header_meta.filament_used_mm > 0) ? header_meta.filament_used_mm : 5400.0;
    double filament_g =
        (header_meta.filament_used_g > 0) ? header_meta.filament_used_g : filament_mm * 0.00298;

    json result = {{"filename", filename},
                   {"size", size},
                   {"modified", modified},
                   {"slicer", header_meta.slicer},
                   {"slicer_version", header_meta.slicer_version},
                   {"estimated_time", estimated_time},
                   {"filament_total", filament_mm},
                   {"filament_weight_total", filament_g},
                   {"filament_type", header_meta.filament_type},
                   {"filament_colors", filament_colors},
                   {"filament_weights", filament_weights},
                   {"layer_count", header_meta.layer_count},
                   {"layer_height", header_meta.layer_height},
                   {"first_layer_height", header_meta.first_layer_height},
                   {"object_height", header_meta.object_height},
                   {"first_layer_bed_temp", header_meta.first_layer_bed_temp},
                   {"first_layer_extr_temp", header_meta.first_layer_nozzle_temp},
                   {"thumbnails", thumbnails}};

    json response = {{"result", result}};

    spdlog::trace("[MoonrakerClientMock] Built metadata for '{}': {}s, {}g filament", filename,
                  header_meta.estimated_time_seconds, header_meta.filament_used_g);
    return response;
}

namespace mock_internal {

void register_file_handlers(std::unordered_map<std::string, MethodHandler>& registry) {
    // server.files.list - List files in a directory
    registry["server.files.list"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)self;
        (void)error_cb;
        if (!success_cb) {
            return true;
        }

        std::string root;
        std::string path;
        if (params.contains("root")) {
            root = params["root"].get<std::string>();
        }
        if (params.contains("path")) {
            path = params["path"].get<std::string>();
        }
        json response = build_mock_file_list_response(root, path);
        spdlog::debug("[MoonrakerClientMock] Returning mock file list for root='{}' path='{}'",
                      root, path.empty() ? "/" : path);
        success_cb(response);
        return true;
    };

    // server.files.get_directory - Get directory contents (same format as list)
    registry["server.files.get_directory"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)self;
        (void)error_cb;
        if (!success_cb) {
            return true;
        }

        std::string root;
        std::string path;
        if (params.contains("root")) {
            root = params["root"].get<std::string>();
        }
        if (params.contains("path")) {
            path = params["path"].get<std::string>();
        }
        json response = build_mock_file_list_response(root, path);
        spdlog::debug(
            "[MoonrakerClientMock] Returning mock directory listing for root='{}' path='{}'", root,
            path.empty() ? "/" : path);
        success_cb(response);
        return true;
    };

    // server.files.metadata - Get file metadata
    registry["server.files.metadata"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)self;
        std::string filename;
        if (params.contains("filename")) {
            filename = params["filename"].get<std::string>();
        }
        if (!filename.empty()) {
            if (success_cb) {
                json response = build_mock_file_metadata_response(filename);
                spdlog::trace("[MoonrakerClientMock] Returning mock metadata for: {}", filename);
                success_cb(response);
            }
        } else if (error_cb) {
            MoonrakerError err = MoonrakerError::validation_error("server.files.metadata",
                                                                  "Missing filename parameter");
            error_cb(err);
        }
        return true;
    };

    // server.files.metascan - Force metadata scan for a file
    // Same as metadata but forces re-parse (in mock, behaves identically)
    registry["server.files.metascan"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)self;
        std::string filename;
        if (params.contains("filename")) {
            filename = params["filename"].get<std::string>();
        }
        if (!filename.empty()) {
            if (success_cb) {
                json response = build_mock_file_metadata_response(filename);
                spdlog::debug("[MoonrakerClientMock] Returning mock metascan for: {}", filename);
                success_cb(response);
            }
        } else if (error_cb) {
            MoonrakerError err = MoonrakerError::validation_error("server.files.metascan",
                                                                  "Missing filename parameter");
            error_cb(err);
        }
        return true;
    };

    // server.files.get_file - Download file content (used for accelerometer CSV data)
    registry["server.files.get_file"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)self;
        if (!success_cb) {
            return true;
        }

        std::string filename;
        if (params.contains("filename")) {
            filename = params["filename"].get<std::string>();
        }

        if (filename.empty()) {
            if (error_cb) {
                MoonrakerError err = MoonrakerError::validation_error("server.files.get_file",
                                                                      "Missing filename parameter");
                error_cb(err);
            }
            return true;
        }

        // Generate mock CSV content for accelerometer data files
        std::string csv_content;
        if (filename.find("raw_data_") != std::string::npos &&
            filename.find(".csv") != std::string::npos) {
            csv_content = "#time,accel_x,accel_y,accel_z\n"
                          "0.000000,0.1,0.2,9.8\n"
                          "0.001000,0.3,0.5,9.7\n"
                          "0.002000,-0.1,0.4,9.8\n"
                          "0.003000,0.2,-0.3,9.9\n"
                          "0.004000,0.5,0.1,9.7\n"
                          "0.005000,-0.2,0.6,9.8\n"
                          "0.006000,0.4,0.3,9.7\n"
                          "0.007000,0.1,-0.2,9.9\n"
                          "0.008000,-0.3,0.4,9.8\n"
                          "0.009000,0.2,0.5,9.7\n";
        } else {
            csv_content = "mock file content for: " + filename;
        }

        json response = {{"result", csv_content}};
        spdlog::debug("[MoonrakerClientMock] Returning mock file content for: {}", filename);
        success_cb(response);
        return true;
    };

    // server.files.delete - Delete a file
    registry["server.files.delete"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)self;
        std::string path;
        if (params.contains("path")) {
            path = params["path"].get<std::string>();
        }
        if (!path.empty()) {
            spdlog::info("[MoonrakerClientMock] Mock delete_file: {}", path);
            if (success_cb) {
                json response = {{"result", {{"item", {{"path", path}, {"root", "gcodes"}}}}}};
                success_cb(response);
            }
        } else if (error_cb) {
            MoonrakerError err =
                MoonrakerError::validation_error("server.files.delete", "Missing path parameter");
            error_cb(err);
        }
        return true;
    };

    // server.files.delete_file - Delete a file
    //
    // What MoonrakerFileAPI::delete_file() sends (the delete flow the print
    // select panel drives). The mock's gcodes root is backed by TEST_GCODE_DIR
    // on disk, so removing the file there IS the virtual-FS mutation — the
    // next get_directory scan no longer reports it. A path that names nothing
    // on disk is an error, like real Moonraker's 404.
    registry["server.files.delete_file"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)self;
        std::string path;
        if (params.contains("path")) {
            path = params["path"].get<std::string>();
        }
        if (path.empty()) {
            if (error_cb) {
                MoonrakerError err = MoonrakerError::validation_error("server.files.delete_file",
                                                                      "Missing path parameter");
                error_cb(err);
            }
            return true;
        }

        // Callers embed the registered root ("gcodes/foo.gcode"); the mock's
        // gcodes tree lives in TEST_GCODE_DIR.
        std::string relative = path;
        const std::string root_prefix = "gcodes/";
        if (relative.rfind(root_prefix, 0) == 0) {
            relative = relative.substr(root_prefix.length());
        }
        const std::string disk_path = std::string(TEST_GCODE_DIR) + "/" + relative;

        spdlog::info("[MoonrakerClientMock] Mock delete_file: {}", path);
        if (std::remove(disk_path.c_str()) == 0) {
            if (success_cb) {
                json response = {{"result", {{"item", {{"path", path}, {"root", "gcodes"}}}}}};
                success_cb(response);
            }
        } else if (error_cb) {
            MoonrakerError err = MoonrakerError::file_not_found("server.files.delete_file",
                                                                "File not found: " + path);
            error_cb(err);
        }
        return true;
    };

    // server.files.move - Move/rename a file
    registry["server.files.move"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)self;
        std::string source, dest;
        if (params.contains("source")) {
            source = params["source"].get<std::string>();
        }
        if (params.contains("dest")) {
            dest = params["dest"].get<std::string>();
        }
        if (!source.empty() && !dest.empty()) {
            spdlog::info("[MoonrakerClientMock] Mock move_file: {} -> {}", source, dest);
            if (success_cb) {
                json response = {{"result", {{"item", {{"path", dest}, {"root", "gcodes"}}}}}};
                success_cb(response);
            }
        } else if (error_cb) {
            MoonrakerError err = MoonrakerError::validation_error(
                "server.files.move", "Missing source or dest parameter");
            error_cb(err);
        }
        return true;
    };

    // server.files.copy - Copy a file
    registry["server.files.copy"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)self;
        std::string source, dest;
        if (params.contains("source")) {
            source = params["source"].get<std::string>();
        }
        if (params.contains("dest")) {
            dest = params["dest"].get<std::string>();
        }
        if (!source.empty() && !dest.empty()) {
            spdlog::info("[MoonrakerClientMock] Mock copy_file: {} -> {}", source, dest);
            if (success_cb) {
                json response = {{"result", {{"item", {{"path", dest}, {"root", "gcodes"}}}}}};
                success_cb(response);
            }
        } else if (error_cb) {
            MoonrakerError err = MoonrakerError::validation_error(
                "server.files.copy", "Missing source or dest parameter");
            error_cb(err);
        }
        return true;
    };

    // server.files.post_directory - Create a directory
    registry["server.files.post_directory"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)self;
        std::string path;
        if (params.contains("path")) {
            path = params["path"].get<std::string>();
        }
        if (!path.empty()) {
            spdlog::info("[MoonrakerClientMock] Mock create_directory: {}", path);
            if (success_cb) {
                json response = {{"result", {{"item", {{"path", path}, {"root", "gcodes"}}}}}};
                success_cb(response);
            }
        } else if (error_cb) {
            MoonrakerError err = MoonrakerError::validation_error("server.files.post_directory",
                                                                  "Missing path parameter");
            error_cb(err);
        }
        return true;
    };

    // server.files.delete_directory - Delete a directory
    registry["server.files.delete_directory"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)self;
        std::string path;
        if (params.contains("path")) {
            path = params["path"].get<std::string>();
        }
        if (!path.empty()) {
            spdlog::info("[MoonrakerClientMock] Mock delete_directory: {}", path);
            if (success_cb) {
                json response = {{"result", {{"item", {{"path", path}, {"root", "gcodes"}}}}}};
                success_cb(response);
            }
        } else if (error_cb) {
            MoonrakerError err = MoonrakerError::validation_error("server.files.delete_directory",
                                                                  "Missing path parameter");
            error_cb(err);
        }
        return true;
    };
}

} // namespace mock_internal
