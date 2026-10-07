// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_api_mock.h"

#include "ui_update_queue.h"

#include "../tests/mocks/mock_printer_state.h"
#include "env_knobs.h"
#include "gcode_parser.h"
#include "mock_planted_gcodes.h"
#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"
#include "plugin_source_app.h"
#include "power_device_state.h"
#include "runtime_config.h"
#include "screws_tilt_parser.h"
#include "sensor_state.h"
#include "text_io.h"
#include "timelapse_state.h"

#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Alias for cleaner code - use shared constant from RuntimeConfig
#define TEST_GCODE_DIR RuntimeConfig::TEST_GCODE_DIR

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <sstream>

using namespace helix;

// Static initialization of path prefixes for fallback search
const std::vector<std::string> MoonrakerFileTransferAPIMock::PATH_PREFIXES = {
    "",      // From project root: assets/test_gcodes/
    "../",   // From build/: ../assets/test_gcodes/
    "../../" // From build/bin/: ../../assets/test_gcodes/
};

MoonrakerFileTransferAPIMock::MoonrakerFileTransferAPIMock(MoonrakerClient& client,
                                                           const std::string& http_base_url)
    : MoonrakerFileTransferAPI(client, http_base_url) {
    spdlog::debug(
        "[MoonrakerFileTransferAPIMock] Created - HTTP methods will use local test files");
}

// ============================================================================
// MoonrakerAdvancedAPIMock Implementation
// ============================================================================

MoonrakerAdvancedAPIMock::MoonrakerAdvancedAPIMock(MoonrakerClient& client, MoonrakerAPI& api)
    : MoonrakerAdvancedAPI(client, api) {}

MoonrakerAPIMock::MoonrakerAPIMock(MoonrakerClient& client, PrinterState& state)
    : MoonrakerAPI(client, state) {
    spdlog::debug("[MoonrakerAPIMock] Created - using mock sub-APIs");

    // Replace base sub-APIs with mock versions
    advanced_api_ = std::make_unique<MoonrakerAdvancedAPIMock>(client, *this);
    file_transfer_api_ =
        std::make_unique<MoonrakerFileTransferAPIMock>(client, get_http_base_url());
    file_api_ = std::make_unique<MoonrakerFileAPIMock>(client);
    job_api_ = std::make_unique<helix::MoonrakerJobAPIMock>(client, &state);
    rest_api_ = std::make_unique<MoonrakerRestAPIMock>(client, get_http_base_url());
    timelapse_api_ = std::make_unique<MoonrakerTimelapseAPIMock>(client, get_http_base_url());
}

MoonrakerAdvancedAPIMock& MoonrakerAPIMock::advanced_mock() {
    return static_cast<MoonrakerAdvancedAPIMock&>(*advanced_api_);
}

MoonrakerFileTransferAPIMock& MoonrakerAPIMock::transfers_mock() {
    return static_cast<MoonrakerFileTransferAPIMock&>(*file_transfer_api_);
}

helix::MoonrakerJobAPIMock& MoonrakerAPIMock::job_mock() {
    return static_cast<helix::MoonrakerJobAPIMock&>(*job_api_);
}

MoonrakerFileAPIMock& MoonrakerAPIMock::files_mock() {
    return static_cast<MoonrakerFileAPIMock&>(*file_api_);
}

void MoonrakerAPIMock::set_config_files(std::map<std::string, std::string> files) {
    files_mock().set_config_files(files);
    transfers_mock().set_config_files(std::move(files));
}

std::optional<std::string> MoonrakerAPIMock::get_uploaded_config(const std::string& path) const {
    return static_cast<const MoonrakerFileTransferAPIMock&>(*file_transfer_api_)
        .get_uploaded_config(path);
}

MoonrakerRestAPIMock& MoonrakerAPIMock::rest_mock() {
    return static_cast<MoonrakerRestAPIMock&>(*rest_api_);
}

MoonrakerTimelapseAPIMock& MoonrakerAPIMock::timelapse_mock() {
    return static_cast<MoonrakerTimelapseAPIMock&>(*timelapse_api_);
}

// ============================================================================
// Connection/Subscription/Database Proxy Overrides (mock no-ops)
// ============================================================================

SubscriptionId
MoonrakerAPIMock::subscribe_notifications(std::function<void(const json&)> /*callback*/) {
    return mock_next_subscription_id_++;
}

bool MoonrakerAPIMock::unsubscribe_notifications(SubscriptionId /*id*/) {
    return true;
}

void MoonrakerAPIMock::register_method_callback(const std::string& method, const std::string& name,
                                                std::function<void(const json&)> callback) {
    // The mock client dispatches notifications itself, so listeners reach them.
    MoonrakerAPI::register_method_callback(method, name, std::move(callback));
}

bool MoonrakerAPIMock::unregister_method_callback(const std::string& method,
                                                  const std::string& name) {
    return MoonrakerAPI::unregister_method_callback(method, name);
}

void MoonrakerAPIMock::suppress_disconnect_modal(uint32_t duration_ms) {
    // No behaviour in the mock; recorded so tests can assert the arm.
    ++suppress_disconnect_modal_calls_;
    last_suppress_disconnect_modal_ms_ = duration_ms;
}

void MoonrakerAPIMock::get_gcode_store(
    int /*count*/, std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
    std::function<void(const MoonrakerError&)> /*on_error*/) {
    if (on_success) {
        double now = static_cast<double>(std::time(nullptr));
        std::vector<GcodeStoreEntry> entries = {
            {"G28", now - 120, "command"},
            {"ok", now - 119, "response"},
            {"G29", now - 100, "command"},
            {"ok", now - 99, "response"},
            {"M104 S210", now - 80, "command"},
            {"ok", now - 79, "response"},
            {"M190 S60", now - 60, "command"},
            {"ok B:58.2 /60.0", now - 55, "response"},
            {"ok B:59.8 /60.0", now - 50, "response"},
            {"ok B:60.0 /60.0", now - 45, "response"},
            {"FIRMWARE_RESTART", now - 30, "command"},
            {"!! Error: MCU protocol error", now - 29, "response"},
            {"RESTART", now - 10, "command"},
            {"ok", now - 9, "response"},
        };
        on_success(entries);
    }
}

std::string MoonrakerFileTransferAPIMock::find_test_file(const std::string& filename) const {
    namespace fs = std::filesystem;

    const std::string& planted = helix::mock::planted_gcode_dir();
    if (!planted.empty() && fs::exists(planted + "/" + filename)) {
        return planted + "/" + filename;
    }

    for (const auto& prefix : PATH_PREFIXES) {
        std::string path = prefix + std::string(TEST_GCODE_DIR) + "/" + filename;

        if (fs::exists(path)) {
            spdlog::debug("[MoonrakerAPIMock] Found test file at: {}", path);
            return path;
        }
    }

    // File not found in any location
    spdlog::debug("[MoonrakerAPIMock] Test file not found in any search path: {}", filename);
    return "";
}

void MoonrakerFileTransferAPIMock::set_config_files(std::map<std::string, std::string> files) {
    config_files_ = std::move(files);
    spdlog::debug("[MoonrakerAPIMock] Injected {} in-memory config files", config_files_.size());
}

std::optional<std::string>
MoonrakerFileTransferAPIMock::get_uploaded_config(const std::string& path) const {
    auto it = config_files_.find(path);
    if (it == config_files_.end())
        return std::nullopt;
    return it->second;
}

std::map<std::string, std::string> MoonrakerFileTransferAPIMock::get_config_files() const {
    return config_files_;
}

MoonrakerFileTransferAPIMock::ConfigRootFile
MoonrakerFileTransferAPIMock::lookup_config_root(const std::string& root,
                                                 const std::string& path) const {
    ConfigRootFile out;
    if (root != "config")
        return out;
    const std::string_view plugin_path(path);
    const char* plugins_dir = std::getenv("HELIX_MOCK_PLUGINS_DIR");
    std::error_code dir_ec;
    if (plugins_dir && *plugins_dir && std::filesystem::is_directory(plugins_dir, dir_ec) &&
        plugin_path.rfind(plugin::kPluginRootPath, 0) == 0) {
        // The plugins directory owns the whole plugin folder: a miss inside it is a
        // not-found, never a fall-through that could resolve some same-basename test file.
        out.owned = true;
        const std::filesystem::path local =
            std::filesystem::path(plugins_dir) /
            plugin_path.substr(std::string_view(plugin::kPluginRootPath).size());
        std::ifstream in(local, std::ios::binary);
        if (in)
            out.content =
                std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return out;
    }
    if (!config_files_.empty()) {
        auto it = config_files_.find(path);
        if (it != config_files_.end())
            out.content = it->second;
        out.owned = true;
    }
    return out;
}

MoonrakerFileAPIMock::MoonrakerFileAPIMock(helix::IMoonrakerClient& client)
    : MoonrakerFileAPI(client) {}

void MoonrakerFileAPIMock::set_config_files(std::map<std::string, std::string> files) {
    config_files_ = std::move(files);
}

void MoonrakerFileAPIMock::list_files(const std::string& root, const std::string& path,
                                      bool recursive, FileListCallback on_success,
                                      ErrorCallback on_error) {
    if (root == "config") {
        std::vector<FileInfo> listing;
        for (const auto& [file_path, content] : config_files_) {
            FileInfo info;
            info.path = file_path;
            auto slash = file_path.rfind('/');
            info.filename = (slash == std::string::npos) ? file_path : file_path.substr(slash + 1);
            info.size = content.size();
            info.is_dir = false;
            listing.push_back(info);
        }

        // HELIX_MOCK_PLUGINS_DIR serves the plugin folder of the config root from a local
        // directory, so a --test run lists and syncs plugins exactly like a printer would.
        // Runs before the fall-through: with no injected files the directory alone still
        // owns the config root.
        const char* plugins_dir = std::getenv("HELIX_MOCK_PLUGINS_DIR");
        bool plugins_dir_active = false;
        std::error_code dir_ec;
        if (plugins_dir && *plugins_dir && std::filesystem::is_directory(plugins_dir, dir_ec)) {
            plugins_dir_active = true;
            std::error_code walk_ec;
            for (std::filesystem::recursive_directory_iterator it(plugins_dir, walk_ec), end;
                 !walk_ec && it != end; it.increment(walk_ec)) {
                std::error_code file_ec;
                if (!it->is_regular_file(file_ec) || file_ec)
                    continue;
                FileInfo info;
                info.path = std::string(plugin::kPluginRootPath) +
                            it->path().lexically_relative(plugins_dir).generic_string();
                info.filename = it->path().filename().string();
                info.size = it->file_size(file_ec);
                std::error_code mtime_ec;
                const auto mtime = std::filesystem::last_write_time(it->path(), mtime_ec);
                if (!mtime_ec)
                    info.modified = std::chrono::duration<double>(mtime.time_since_epoch()).count();
                info.is_dir = false;
                listing.push_back(info);
            }
            spdlog::debug("[MoonrakerAPIMock] HELIX_MOCK_PLUGINS_DIR={} listed into config root",
                          plugins_dir);
        }

        if (!config_files_.empty() || plugins_dir_active) {
            spdlog::debug("[MoonrakerAPIMock] list_files(config) serving {} mock files",
                          listing.size());
            if (on_success)
                on_success(listing);
            return;
        }
    }

    MoonrakerFileAPI::list_files(root, path, recursive, std::move(on_success), std::move(on_error));
}

void MoonrakerFileAPIMock::delete_file(const std::string& filename, SuccessCallback on_success,
                                       ErrorCallback on_error) {
    (void)on_error; // Unused - the mock always accepts a delete
    deleted_files_.push_back(filename);
    spdlog::debug("[MoonrakerAPIMock] Mock delete_file: {}", filename);
    if (on_success) {
        on_success();
    }
}

// ============================================================================
// MoonrakerJobAPIMock
// ============================================================================

helix::MoonrakerJobAPIMock::MoonrakerJobAPIMock(helix::IMoonrakerClient& client,
                                                const helix::PrinterState* state)
    : MoonrakerJobAPI(client, state) {}

void helix::MoonrakerJobAPIMock::start_print(const std::string& filename,
                                             SuccessCallback on_success, ErrorCallback on_error) {
    started_prints_.push_back(filename);
    MoonrakerJobAPI::start_print(filename, std::move(on_success), std::move(on_error));
}

void helix::MoonrakerJobAPIMock::start_modified_print(const std::string& original_filename,
                                                      const std::string& temp_file_path,
                                                      const std::vector<std::string>& modifications,
                                                      ModifiedPrintCallback on_success,
                                                      ErrorCallback on_error) {
    if (refused_by_spool_latch("server.helix.print_modified", on_error)) {
        return;
    }
    modified_prints_.push_back({original_filename, temp_file_path, modifications});

    spdlog::info("[MoonrakerAPIMock] Mock start_modified_print: original='{}', temp='{}', "
                 "{} modification(s)",
                 original_filename, temp_file_path, modifications.size());

    if (fail_modified_prints_) {
        if (on_error) {
            on_error(MoonrakerError::unknown("Mock plugin rejected: " + temp_file_path,
                                             "start_modified_print"));
        }
        return;
    }

    if (on_success) {
        ModifiedPrintResult result;
        result.original_filename = original_filename;
        result.print_filename = original_filename;
        result.temp_filename = temp_file_path;
        result.status = "printing";
        on_success(result);
    }
}

void MoonrakerFileTransferAPIMock::download_file(const std::string& root, const std::string& path,
                                                 StringCallback on_success,
                                                 ErrorCallback on_error) {
    // Injected config root: resolve the FULL relative path, never the basename.
    // conf.d/options.cfg and macros/options.cfg are different files.
    if (root == "config" && !config_files_.empty()) {
        auto it = config_files_.find(path);
        if (it != config_files_.end()) {
            spdlog::debug("[MoonrakerAPIMock] Serving injected config file: {}", path);
            if (on_success)
                on_success(it->second);
            return;
        }
        spdlog::debug("[MoonrakerAPIMock] Injected config root has no file: {}", path);
        if (on_error) {
            on_error(MoonrakerError::file_not_found("download_file",
                                                    "Mock config file not found: " + path));
        }
        return;
    }

    // Strip any leading directory components to get just the filename
    std::string filename = path;
    size_t last_slash = path.rfind('/');
    if (last_slash != std::string::npos) {
        filename = path.substr(last_slash + 1);
    }

    spdlog::debug("[MoonrakerAPIMock] download_file: root='{}', path='{}' -> filename='{}'", root,
                  path, filename);

    // Find the test file using fallback path search
    std::string local_path;

    // For timelapse root, search timelapse test directory first
    if (root == "timelapse") {
        for (const auto& prefix : PATH_PREFIXES) {
            std::string timelapse_path = prefix + "assets/test_timelapse/" + filename;
            if (std::filesystem::exists(timelapse_path)) {
                local_path = timelapse_path;
                spdlog::debug("[MoonrakerAPIMock] Found timelapse test file at: {}",
                              timelapse_path);
                break;
            }
        }
    }

    if (local_path.empty()) {
        local_path = find_test_file(filename);
    }

    if (local_path.empty()) {
        // File not found in test directory
        spdlog::warn("[MoonrakerAPIMock] File not found in test directories: {}", filename);

        if (on_error) {
            MoonrakerError err =
                MoonrakerError::file_not_found("download_file", "Mock file not found: " + filename);
            on_error(err);
        }
        return;
    }

    // Try to read the local file
    std::ifstream file(local_path, std::ios::binary);
    if (file) {
        std::ostringstream content;
        content << file.rdbuf();
        file.close();

        spdlog::info("[MoonrakerAPIMock] Downloaded {} ({} bytes)", filename, content.str().size());

        if (on_success) {
            on_success(content.str());
        }
    } else {
        // Shouldn't happen if find_test_file succeeded, but handle gracefully
        spdlog::error("[MoonrakerAPIMock] Failed to read file that exists: {}", local_path);

        if (on_error) {
            MoonrakerError err = MoonrakerError::file_not_found(
                "download_file", "Failed to read test file: " + filename);
            on_error(err);
        }
    }
}

void MoonrakerFileTransferAPIMock::download_file_partial(const std::string& root,
                                                         const std::string& path, size_t max_bytes,
                                                         StringCallback on_success,
                                                         ErrorCallback on_error,
                                                         CancelFlag /*cancelled*/) {
    spdlog::debug("[MoonrakerAPIMock] download_file_partial: root='{}', path='{}', max_bytes={}",
                  root, path, max_bytes);

    // Config root: serve HELIX_MOCK_PLUGINS_DIR and the injected config root by full path,
    // mirroring the real partial download's head-range contract.
    const ConfigRootFile config = lookup_config_root(root, path);
    if (config.owned) {
        if (!config.content) {
            if (on_error)
                on_error(MoonrakerError::file_not_found("download_file_partial",
                                                        "Mock config file not found: " + path));
            return;
        }
        if (on_success)
            on_success(config.content->substr(0, max_bytes));
        return;
    }

    // Strip any leading directory components to get just the filename
    std::string filename = path;
    size_t last_slash = path.rfind('/');
    if (last_slash != std::string::npos) {
        filename = path.substr(last_slash + 1);
    }

    // Find the test file using fallback path search
    std::string local_path = find_test_file(filename);

    if (local_path.empty()) {
        spdlog::warn("[MoonrakerAPIMock] File not found in test directories: {}", filename);
        if (on_error) {
            MoonrakerError err = MoonrakerError::file_not_found("download_file_partial",
                                                                "Mock file not found: " + filename);
            on_error(err);
        }
        return;
    }

    // Read up to max_bytes from the local file
    std::ifstream file(local_path, std::ios::binary);
    if (file) {
        std::string content;
        content.resize(max_bytes);
        file.read(&content[0], static_cast<std::streamsize>(max_bytes));
        content.resize(static_cast<size_t>(file.gcount()));
        file.close();

        spdlog::debug("[MoonrakerAPIMock] Partial download {} ({} of {} bytes)", filename,
                      content.size(), max_bytes);

        if (on_success) {
            on_success(content);
        }
    } else {
        spdlog::error("[MoonrakerAPIMock] Failed to read file: {}", local_path);
        if (on_error) {
            MoonrakerError err = MoonrakerError::file_not_found(
                "download_file_partial", "Failed to read test file: " + filename);
            on_error(err);
        }
    }
}

void MoonrakerFileTransferAPIMock::download_file_tail(const std::string& root,
                                                      const std::string& path, size_t max_bytes,
                                                      StringCallback on_success,
                                                      ErrorCallback on_error) {
    std::string filename = path;
    size_t last_slash = path.rfind('/');
    if (last_slash != std::string::npos) {
        filename = path.substr(last_slash + 1);
    }

    spdlog::debug("[MoonrakerAPIMock] download_file_tail: root='{}', path='{}', max_bytes={}", root,
                  path, max_bytes);

    std::string local_path = find_test_file(filename);
    if (local_path.empty()) {
        spdlog::warn("[MoonrakerAPIMock] File not found in test directories: {}", filename);
        if (on_error) {
            on_error(MoonrakerError::file_not_found("download_file_tail",
                                                    "Mock file not found: " + filename));
        }
        return;
    }

    std::ifstream file(local_path, std::ios::binary | std::ios::ate);
    if (!file) {
        spdlog::error("[MoonrakerAPIMock] Failed to read file: {}", local_path);
        if (on_error) {
            on_error(MoonrakerError::file_not_found("download_file_tail",
                                                    "Failed to read test file: " + filename));
        }
        return;
    }

    // Mirror the suffix-range contract: a file shorter than max_bytes yields the
    // whole file, never a short read from a negative offset.
    const auto size = static_cast<size_t>(file.tellg());
    const size_t want = std::min(max_bytes, size);
    file.seekg(static_cast<std::streamoff>(size - want), std::ios::beg);

    std::string content;
    content.resize(want);
    if (want > 0) {
        file.read(&content[0], static_cast<std::streamsize>(want));
        content.resize(static_cast<size_t>(file.gcount()));
    }
    file.close();

    spdlog::debug("[MoonrakerAPIMock] Tail download {} (last {} of {} bytes)", filename,
                  content.size(), size);

    if (on_success) {
        on_success(content);
    }
}

void MoonrakerFileTransferAPIMock::download_file_to_path(
    const std::string& root, const std::string& path, const std::string& dest_path,
    StringCallback on_success, ErrorCallback on_error, ProgressCallback on_progress) {
    (void)on_progress; // Progress callback ignored in mock
    download_destinations_.push_back(dest_path);

    // Config root: serve HELIX_MOCK_PLUGINS_DIR and the injected config root by full path,
    // so a config download lands on disk exactly what the in-memory root holds.
    const ConfigRootFile config = lookup_config_root(root, path);
    if (config.owned) {
        if (!config.content) {
            spdlog::warn("[MoonrakerAPIMock] File not found in config root: {}", path);
            if (on_error) {
                on_error(MoonrakerError::file_not_found("download_file_to_path",
                                                        "Mock config file not found: " + path));
            }
            return;
        }
        if (!text_io::write_file(dest_path, *config.content)) {
            spdlog::error("[MoonrakerAPIMock] Failed to create destination file: {}", dest_path);
            if (on_error) {
                on_error(MoonrakerError::unknown("Failed to create destination file: " + dest_path,
                                                 "download_file_to_path"));
            }
            return;
        }
        if (on_success)
            on_success(dest_path);
        return;
    }

    // Extract just the filename from the path
    std::string filename = path;
    size_t last_slash = path.find_last_of('/');
    if (last_slash != std::string::npos) {
        filename = path.substr(last_slash + 1);
    }

    spdlog::debug(
        "[MoonrakerAPIMock] download_file_to_path: root='{}', path='{}' -> filename='{}', "
        "dest='{}'",
        root, path, filename, dest_path);

    // Find the test file using fallback path search
    std::string local_path;

    // For timelapse root, search timelapse test directory first
    if (root == "timelapse") {
        for (const auto& prefix : PATH_PREFIXES) {
            std::string timelapse_path = prefix + "assets/test_timelapse/" + filename;
            if (std::filesystem::exists(timelapse_path)) {
                local_path = timelapse_path;
                spdlog::debug("[MoonrakerAPIMock] Found timelapse test file at: {}",
                              timelapse_path);
                break;
            }
        }
    }

    if (local_path.empty()) {
        local_path = find_test_file(filename);
    }

    if (local_path.empty()) {
        spdlog::warn("[MoonrakerAPIMock] File not found in test directories: {}", filename);

        if (on_error) {
            MoonrakerError err = MoonrakerError::file_not_found("download_file_to_path",
                                                                "Mock file not found: " + filename);
            on_error(err);
        }
        return;
    }

    // Copy the file to destination
    std::ifstream src(local_path, std::ios::binary);
    if (!src) {
        spdlog::error("[MoonrakerAPIMock] Failed to open source file: {}", local_path);
        if (on_error) {
            MoonrakerError err = MoonrakerError::file_not_found(
                "download_file_to_path", "Failed to read test file: " + filename);
            on_error(err);
        }
        return;
    }

    std::ofstream dst(dest_path, std::ios::binary);
    if (!dst) {
        spdlog::error("[MoonrakerAPIMock] Failed to create destination file: {}", dest_path);
        if (on_error) {
            MoonrakerError err = MoonrakerError::unknown(
                "Failed to create destination file: " + dest_path, "download_file_to_path");
            on_error(err);
        }
        return;
    }

    dst << src.rdbuf();
    src.close();
    dst.close();

    // Verify the copy worked
    auto file_size = std::filesystem::file_size(dest_path);
    spdlog::debug("[MoonrakerAPIMock] Copied {} -> {} ({} bytes)", local_path, dest_path,
                  file_size);

    if (on_success) {
        on_success(dest_path);
    }
}

void MoonrakerFileTransferAPIMock::upload_file_from_path(
    const std::string& root, const std::string& dest_path, const std::string& local_path,
    SuccessCallback on_success, ErrorCallback on_error, ProgressCallback on_progress) {
    (void)on_progress; // Progress callback ignored in mock

    // Snapshot the bytes now: the producer deletes local_path from its own
    // success callback, so a test that reads the file afterwards finds nothing.
    PathUploadRecord record;
    record.root = root;
    record.dest_path = dest_path;
    record.local_path = local_path;
    {
        std::ifstream src(local_path, std::ios::binary);
        if (src) {
            record.content.assign(std::istreambuf_iterator<char>(src),
                                  std::istreambuf_iterator<char>());
        }
    }
    path_uploads_.push_back(record);

    spdlog::info("[MoonrakerAPIMock] Mock upload_file_from_path: root='{}', dest='{}', "
                 "local='{}', size={} bytes",
                 root, dest_path, local_path, record.content.size());

    if (hold_path_uploads_) {
        held_path_uploads_.push_back(std::move(on_success));
        return;
    }

    if (fail_path_uploads_) {
        if (on_error) {
            on_error(MoonrakerError::unknown("Mock upload rejected: " + dest_path,
                                             "upload_file_from_path"));
        }
        return;
    }

    if (on_success) {
        on_success();
    }
}

void MoonrakerFileTransferAPIMock::upload_file(const std::string& root, const std::string& path,
                                               const std::string& content,
                                               SuccessCallback on_success, ErrorCallback on_error) {
    (void)on_error; // Unused - mock always succeeds

    spdlog::info("[MoonrakerAPIMock] Mock upload_file: root='{}', path='{}', size={} bytes", root,
                 path, content.size());

    // Record writes to the injected config root so tests can assert on what a
    // config edit actually wrote, and so a later download reads it back.
    if (root == "config" && !config_files_.empty()) {
        config_files_[path] = content;
    }

    // Mock always succeeds
    if (on_success) {
        on_success();
    }
}

void MoonrakerFileTransferAPIMock::upload_file_with_name(
    const std::string& root, const std::string& path, const std::string& filename,
    const std::string& content, SuccessCallback on_success, ErrorCallback on_error) {
    (void)on_error; // Unused - mock always succeeds

    spdlog::info(
        "[MoonrakerAPIMock] Mock upload_file_with_name: root='{}', path='{}', filename='{}', "
        "size={} bytes",
        root, path, filename, content.size());

    // Same recording contract as upload_file(): an injected config root
    // receives the write, so tests can assert on what a config edit actually
    // wrote. An empty path is the config root, so the file lands at the bare
    // filename; a subdirectory prefixes it.
    if (root == "config" && !config_files_.empty()) {
        config_files_[path.empty() ? filename : path + "/" + filename] = content;
    }

    // Mock always succeeds
    if (on_success) {
        on_success();
    }
}

void MoonrakerFileTransferAPIMock::download_thumbnail(const std::string& thumbnail_path,
                                                      const std::string& cache_path,
                                                      StringCallback on_success,
                                                      ErrorCallback on_error) {
    spdlog::debug("[MoonrakerAPIMock] download_thumbnail: path='{}' -> cache='{}'", thumbnail_path,
                  cache_path);

    // HELIX_MOCK_REMOTE_THUMBS=1 — go through the REAL transfer implementation
    // so the request actually crosses HTTP to MockHttpFileServer. Resolving the
    // file locally here is faster and is the right default, but it means the
    // download → HttpExecutor worker → decode → prescale → evict pipeline is
    // never executed under --test, which is the pipeline bundle 6F3QJLFG
    // implicates (#960). This is the only way to reach it without a printer.
    static const bool remote_thumbs = [] { return helix::env_flag("HELIX_MOCK_REMOTE_THUMBS"); }();
    if (remote_thumbs) {
        MoonrakerFileTransferAPI::download_thumbnail(thumbnail_path, cache_path,
                                                     std::move(on_success), std::move(on_error));
        return;
    }

    (void)on_error; // Unused below - mock falls back to placeholder on failure

    namespace fs = std::filesystem;

    // First check: if thumbnail_path is already a local file that exists, use it directly
    // This handles paths like "build/thumbnail_cache/filename.png" from mock metadata
    if (fs::exists(thumbnail_path)) {
        try {
            // Copy to cache path (unless they're the same)
            if (thumbnail_path != cache_path) {
                fs::copy_file(thumbnail_path, cache_path, fs::copy_options::overwrite_existing);
            }
            spdlog::info("[MoonrakerAPIMock] Using local thumbnail {} -> {}", thumbnail_path,
                         cache_path);
            if (on_success) {
                on_success("A:" + cache_path);
            }
            return;
        } catch (const fs::filesystem_error& e) {
            spdlog::warn("[MoonrakerAPIMock] Failed to copy local thumbnail: {}", e.what());
            // Fall through to other methods
        }
    }

    // Moonraker thumbnail paths look like: ".thumbnails/filename-NNxNN.png"
    // Try to find the corresponding G-code file and extract the thumbnail
    std::string gcode_filename;

    // Extract the G-code filename from the thumbnail path
    // e.g., ".thumbnails/3DBenchy-300x300.png" -> "3DBenchy.gcode"
    size_t thumb_start = thumbnail_path.find(".thumbnails/");
    if (thumb_start != std::string::npos) {
        std::string thumb_name = thumbnail_path.substr(thumb_start + 12);
        // Remove resolution suffix like "-300x300.png" or "_300x300.png"
        size_t dash = thumb_name.rfind('-');
        size_t underscore = thumb_name.rfind('_');
        size_t sep = (dash != std::string::npos) ? dash : underscore;
        if (sep != std::string::npos) {
            gcode_filename = thumb_name.substr(0, sep) + ".gcode";
        }
    }

    // Try to find and extract thumbnail from the G-code file
    if (!gcode_filename.empty()) {
        std::string gcode_path = find_test_file(gcode_filename);
        if (!gcode_path.empty()) {
            const auto thumb = helix::gcode::get_best_thumbnail(gcode_path);
            if (!thumb.png_data.empty()) {
                // Write the thumbnail to the cache path
                std::ofstream file(cache_path, std::ios::binary);
                if (file) {
                    file.write(reinterpret_cast<const char*>(thumb.png_data.data()),
                               static_cast<std::streamsize>(thumb.png_data.size()));
                    file.close();

                    spdlog::info(
                        "[MoonrakerAPIMock] Extracted thumbnail {}x{} ({} bytes) from {} -> {}",
                        thumb.width, thumb.height, thumb.png_data.size(), gcode_filename,
                        cache_path);

                    if (on_success) {
                        on_success(cache_path);
                    }
                    return;
                }
            } else {
                spdlog::debug("[MoonrakerAPIMock] No thumbnails found in {}", gcode_path);
            }
        } else {
            spdlog::debug("[MoonrakerAPIMock] G-code file not found: {}", gcode_filename);
        }
    }

    // Fallback to placeholder if extraction failed
    spdlog::debug("[MoonrakerAPIMock] Falling back to placeholder thumbnail");

    std::string placeholder_path;
    for (const auto& prefix : PATH_PREFIXES) {
        std::string test_path = prefix + "assets/images/benchy_thumbnail_white.png";
        if (fs::exists(test_path)) {
            placeholder_path = "A:" + test_path;
            break;
        }
    }

    if (placeholder_path.empty()) {
        placeholder_path = "A:assets/images/placeholder_thumbnail.png";
    }

    if (on_success) {
        on_success(placeholder_path);
    }
}

// ============================================================================
// Power Device Methods
// ============================================================================

void MoonrakerAPIMock::get_power_devices(PowerDevicesCallback on_success, ErrorCallback on_error) {
    (void)on_error; // Mock never fails

    // Test empty state with: MOCK_EMPTY_POWER=1
    if (helix::env_flag("MOCK_EMPTY_POWER")) {
        spdlog::info("[MoonrakerAPIMock] Returning empty power devices (MOCK_EMPTY_POWER set)");
        on_success({});
        return;
    }

    spdlog::info("[MoonrakerAPIMock] Returning mock power devices");

    // Initialize mock states if not already done
    if (mock_power_states_.empty()) {
        mock_power_states_["printer_psu"] = true;
        mock_power_states_["led_strip"] = true;
        mock_power_states_["enclosure_fan"] = false;
        mock_power_states_["aux_outlet"] = false;
    }

    // Create mock device list that mimics real Moonraker responses
    std::vector<PowerDevice> devices;

    // Printer PSU - typically locked during printing
    devices.push_back({
        "printer_psu",                                    // device name
        "gpio",                                           // type
        mock_power_states_["printer_psu"] ? "on" : "off", // status
        true                                              // locked_while_printing
    });

    // LED Strip - controllable anytime
    devices.push_back({"led_strip", "gpio", mock_power_states_["led_strip"] ? "on" : "off", false});

    // Enclosure Fan - controllable anytime
    devices.push_back({"enclosure_fan", "klipper_device",
                       mock_power_states_["enclosure_fan"] ? "on" : "off", false});

    // Auxiliary Outlet
    devices.push_back(
        {"aux_outlet", "tplink_smartplug", mock_power_states_["aux_outlet"] ? "on" : "off", false});

    if (on_success) {
        on_success(devices);
    }
}

void MoonrakerAPIMock::set_device_power(const std::string& device, const std::string& action,
                                        SuccessCallback on_success, ErrorCallback on_error) {
    (void)on_error; // Mock never fails

    // Update mock state
    bool new_state = false;
    if (action == "on") {
        new_state = true;
    } else if (action == "off") {
        new_state = false;
    } else if (action == "toggle") {
        new_state = !mock_power_states_[device];
    }

    mock_power_states_[device] = new_state;

    spdlog::info("[MoonrakerAPIMock] Power device '{}' set to '{}' (state: {})", device, action,
                 new_state ? "on" : "off");

    if (on_success) {
        on_success();
    }

    // Simulate notify_power_changed so PowerDeviceState updates subjects
    helix::PowerDeviceState::instance().update_device_status(device, new_state ? "on" : "off");
}

// ============================================================================
// Sensor Mock
// ============================================================================

void MoonrakerAPIMock::get_sensors(SensorsCallback on_success, ErrorCallback /*on_error*/) {
    spdlog::info("[MoonrakerAPIMock] Returning mock sensors");

    std::vector<helix::SensorInfo> sensors = {
        {"mock_energy", "Mock Energy Monitor", "mqtt", {"power", "voltage", "current", "energy"}},
    };

    nlohmann::json initial_values = {
        {"mock_energy",
         {{"power", 45.0}, {"voltage", 230.5}, {"current", 0.195}, {"energy", 123.4}}},
    };

    if (on_success) {
        on_success(sensors, initial_values);
    }
}

// ============================================================================
// MoonrakerRestAPIMock Implementation
// ============================================================================

MoonrakerRestAPIMock::MoonrakerRestAPIMock(MoonrakerClient& client,
                                           const std::string& http_base_url)
    : MoonrakerRestAPI(client, http_base_url) {}

void MoonrakerRestAPIMock::mock_release_wled_strips() {
    hold_wled_strips_ = false;
    auto held = std::move(held_wled_strips_);
    held_wled_strips_.clear();
    for (auto& answer : held) {
        answer();
    }
}

void MoonrakerRestAPIMock::wled_get_strips(RestCallback on_success, ErrorCallback on_error) {
    if (hold_wled_strips_) {
        held_wled_strips_.push_back(
            [this, on_success, on_error]() { wled_get_strips(on_success, on_error); });
        return;
    }
    spdlog::info("[MoonrakerAPIMock] WLED get_strips (returning mock strips from tracked state)");

    // Initialize defaults if not already set (same pattern as wled_get_status)
    if (mock_wled_states_.find("printer_led") == mock_wled_states_.end()) {
        mock_wled_states_["printer_led"] = true;
    }
    if (mock_wled_states_.find("enclosure_led") == mock_wled_states_.end()) {
        mock_wled_states_["enclosure_led"] = false;
    }
    if (mock_wled_presets_.find("printer_led") == mock_wled_presets_.end()) {
        mock_wled_presets_["printer_led"] = 2;
    }
    if (mock_wled_presets_.find("enclosure_led") == mock_wled_presets_.end()) {
        mock_wled_presets_["enclosure_led"] = -1;
    }
    if (mock_wled_brightness_.find("printer_led") == mock_wled_brightness_.end()) {
        mock_wled_brightness_["printer_led"] = 200;
    }
    if (mock_wled_brightness_.find("enclosure_led") == mock_wled_brightness_.end()) {
        mock_wled_brightness_["enclosure_led"] = 128;
    }

    if (on_success) {
        RestResponse resp;
        resp.success = true;
        resp.status_code = 200;
        // Moonraker nests the strip map under result.strips — reproduce that envelope
        // exactly, or parser bugs that read the wrapper key as a strip name go
        // undetected (prestonbrown/helixscreen#1241).
        resp.data = {{"result",
                      {{"strips",
                        {{"printer_led",
                          {{"strip", "printer_led"},
                           {"status", mock_wled_states_["printer_led"] ? "on" : "off"},
                           {"brightness", mock_wled_brightness_["printer_led"]},
                           {"preset", mock_wled_presets_["printer_led"]}}},
                         {"enclosure_led",
                          {{"strip", "enclosure_led"},
                           {"status", mock_wled_states_["enclosure_led"] ? "on" : "off"},
                           {"brightness", mock_wled_brightness_["enclosure_led"]},
                           {"preset", mock_wled_presets_["enclosure_led"]}}}}}}}};
        on_success(resp);
    }
}

void MoonrakerRestAPIMock::wled_set_strip(const std::string& strip, const std::string& action,
                                          int brightness, int preset, SuccessCallback on_success,
                                          ErrorCallback /*on_error*/) {
    spdlog::info("[MoonrakerAPIMock] WLED set_strip: strip={} action={} brightness={} preset={}",
                 strip, action, brightness, preset);

    // Track on/off/toggle state for status polling
    if (action == "on") {
        mock_wled_states_[strip] = true;
    } else if (action == "off") {
        mock_wled_states_[strip] = false;
    } else if (action == "toggle") {
        mock_wled_states_[strip] = !mock_wled_states_[strip];
    }

    // Track brightness changes
    if (brightness >= 0) {
        mock_wled_brightness_[strip] = brightness;
    }

    // Track active preset
    if (preset >= 0) {
        mock_wled_presets_[strip] = preset;
        mock_wled_states_[strip] = true; // activating a preset turns strip on
    }

    if (on_success) {
        on_success();
    }
}

void MoonrakerRestAPIMock::wled_get_status(RestCallback on_success, ErrorCallback /*on_error*/) {
    spdlog::info("[MoonrakerAPIMock] WLED get_status");

    // Initialize default states if not already set
    if (mock_wled_states_.find("printer_led") == mock_wled_states_.end()) {
        mock_wled_states_["printer_led"] = true;
    }
    if (mock_wled_states_.find("enclosure_led") == mock_wled_states_.end()) {
        mock_wled_states_["enclosure_led"] = false;
    }
    // Default presets
    if (mock_wled_presets_.find("printer_led") == mock_wled_presets_.end()) {
        mock_wled_presets_["printer_led"] = 2;
    }
    if (mock_wled_presets_.find("enclosure_led") == mock_wled_presets_.end()) {
        mock_wled_presets_["enclosure_led"] = -1;
    }
    // Default brightness
    if (mock_wled_brightness_.find("printer_led") == mock_wled_brightness_.end()) {
        mock_wled_brightness_["printer_led"] = 200;
    }
    if (mock_wled_brightness_.find("enclosure_led") == mock_wled_brightness_.end()) {
        mock_wled_brightness_["enclosure_led"] = 128;
    }

    if (on_success) {
        RestResponse resp;
        resp.success = true;
        resp.status_code = 200;
        // Same endpoint as wled_get_strips(), same result.strips envelope.
        resp.data = {{"result",
                      {{"strips",
                        {{"printer_led",
                          {{"strip", "printer_led"},
                           {"status", mock_wled_states_["printer_led"] ? "on" : "off"},
                           {"chain_count", 30},
                           {"preset", mock_wled_presets_["printer_led"]},
                           {"brightness", mock_wled_brightness_["printer_led"]},
                           {"intensity", -1},
                           {"speed", -1},
                           {"error", nullptr}}},
                         {"enclosure_led",
                          {{"strip", "enclosure_led"},
                           {"status", mock_wled_states_["enclosure_led"] ? "on" : "off"},
                           {"chain_count", 60},
                           {"preset", mock_wled_presets_["enclosure_led"]},
                           {"brightness", mock_wled_brightness_["enclosure_led"]},
                           {"intensity", -1},
                           {"speed", -1},
                           {"error", nullptr}}}}}}}};
        on_success(resp);
    }
}

namespace {
const char* mock_klippy_uds_address(); // defined below, beside its socket setup
}

void MoonrakerRestAPIMock::get_server_config(RestCallback on_success, ErrorCallback /*on_error*/) {
    spdlog::info("[MoonrakerAPIMock] get_server_config");

    if (on_success) {
        RestResponse resp;
        resp.success = true;
        resp.status_code = 200;
        resp.data = {
            {"result",
             {{"config",
               {{"server", {{"klippy_uds_address", mock_klippy_uds_address()}}},
                {"wled printer_led",
                 {{"type", "http"}, {"address", "192.168.1.50"}, {"initial_preset", -1}}},
                {"wled enclosure_led",
                 {{"type", "http"}, {"address", "192.168.1.51"}, {"initial_preset", -1}}}}}}}};
        on_success(resp);
    }
}

// ============================================================================
// Mock klippy UDS
// ============================================================================

namespace {

/// Owns the mock klippy socket; unlinks the path at process exit.
struct SocketGuard {
    std::string path;
    int fd;
    ~SocketGuard() {
        ::unlink(path.c_str());
        ::close(fd);
    }
};

/// A process-lifetime listening unix socket standing in for klippy's UDS.
/// Co-location checks connect() to the address Moonraker reports, so the mock
/// must name a socket that accepts a connection. Created once per process on
/// first use and unlinked at exit.
const char* mock_klippy_uds_address() {
    static const std::string path = [] {
        const std::string p = "/tmp/helix-mock-klippy-" + std::to_string(getpid()) + ".sock";
        ::unlink(p.c_str());
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, p.c_str(), sizeof(addr.sun_path) - 1);
            // SOMAXCONN, never 1: connect_uds() blocks on a full backlog, and
            // nothing ever accepts here - a tiny backlog would wedge the
            // second co-location probe for the process lifetime.
            if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0 &&
                ::listen(fd, SOMAXCONN) == 0) {
                static_cast<void>(fcntl(fd, F_SETFD, FD_CLOEXEC));
                // Local static: its destructor unlinks at process exit.
                static const SocketGuard guard{p, fd};
                return p;
            }
            ::close(fd);
        }
        return std::string("/nonexistent/mock-klippy.sock");
    }();
    return path.c_str();
}

} // namespace

// ============================================================================
// Shared State Methods
// ============================================================================

void MoonrakerAPIMock::set_mock_state(std::shared_ptr<MockPrinterState> state) {
    mock_state_ = state;
    if (state) {
        spdlog::debug("[MoonrakerAPIMock] Shared mock state attached");
    } else {
        spdlog::debug("[MoonrakerAPIMock] Shared mock state detached");
    }
}

std::set<std::string> MoonrakerAPIMock::get_excluded_objects_from_mock() const {
    if (mock_state_) {
        return mock_state_->get_excluded_objects();
    }
    return {};
}

std::vector<std::string> MoonrakerAPIMock::get_available_objects_from_mock() const {
    if (mock_state_) {
        return mock_state_->get_available_objects();
    }
    return {};
}

// ============================================================================
// MockScrewsTiltState Implementation
// ============================================================================

MockScrewsTiltState::MockScrewsTiltState() {
    reset();
}

void MockScrewsTiltState::reset() {
    probe_count_ = 0;

    // Initialize 4-corner bed with realistic out-of-level deviations.
    //
    // Offsets are mm above (positive) or below (negative) the nominal plane.
    // Klipper reports diff = z_base - z, so a screw *above* the base prints a
    // negative diff, which is CCW on a CW-M3 thread. The adjustments below are
    // what probe_lines() emits at reset for the 0.5 mm M3 pitch the mock
    // advertises (mock_internal::MOCK_SCREW_THREAD).
    //
    // The base deliberately sits mid-range (-18 .. +10 minutes around it): that
    // is the prestonbrown/helixscreen#1225 shape, where every screw is small on
    // its own but the corner-to-corner spread is 28 minutes.
    screws_ = {
        {"front_left", 30.0f, 30.0f, 0.0f, true},      // Base screw (Klipper's reference)
        {"front_right", 200.0f, 30.0f, 0.15f, false},  // Above base: adjust CCW 00:18
        {"rear_right", 200.0f, 200.0f, -0.08f, false}, // Below base: adjust CW 00:10
        {"rear_left", 30.0f, 200.0f, 0.12f, false}     // Above base: adjust CCW 00:14
    };

    spdlog::debug("[MockScrewsTilt] Reset bed to initial out-of-level state");
}

std::vector<std::string> MockScrewsTiltState::probe_lines() {
    probe_count_++;

    std::vector<std::string> lines;
    lines.reserve(screws_.size() + 1);

    // Klipper prints this legend before every set of screw lines. It carries no
    // " :" separator, so parse_screws_tilt_line() must reject it — keeping it in
    // the mock output means --test exercises that skip path.
    lines.emplace_back(
        "// 01:20 means 1 full turn and 20 minutes, CW=clockwise, CCW=counter-clockwise");

    // Reference Z height (simulated probe at reference screw)
    const float base_z = 2.50f;
    // Klipper takes the FIRST screw in config order as the base, so its probed
    // z is the reference every other screw's diff is measured against.
    float z_base = base_z;
    for (const auto& screw : screws_) {
        if (screw.is_reference) {
            z_base = base_z + screw.current_offset;
            break;
        }
    }

    char buf[160];
    for (const auto& screw : screws_) {
        const float z = base_z + screw.current_offset;
        if (screw.is_reference) {
            snprintf(buf, sizeof(buf), "// %s (base) : x=%.1f, y=%.1f, z=%.5f", screw.name.c_str(),
                     screw.x_pos, screw.y_pos, z);
        } else {
            snprintf(buf, sizeof(buf), "// %s : x=%.1f, y=%.1f, z=%.5f : adjust %s",
                     screw.name.c_str(), screw.x_pos, screw.y_pos, z,
                     diff_to_adjustment(z_base - z).c_str());
        }
        lines.emplace_back(buf);
    }

    spdlog::info("[MockScrewsTilt] Probe #{}: {} screws measured, bed spread {:.3f}mm ({})",
                 probe_count_, screws_.size(), spread_mm(), is_level() ? "level" : "out of level");
    for (const auto& line : lines) {
        spdlog::debug("  {}", line);
    }

    return lines;
}

void MockScrewsTiltState::simulate_user_adjustments() {
    // Use a random number generator for realistic imperfect adjustments
    static std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> correction_dist(0.70f, 0.95f);
    std::uniform_real_distribution<float> noise_dist(-0.005f, 0.005f);

    for (auto& screw : screws_) {
        if (screw.is_reference) {
            continue; // Reference screw is never adjusted
        }

        // User corrects 70-95% of the deviation
        float correction_factor = correction_dist(rng);
        float new_offset = screw.current_offset * (1.0f - correction_factor);

        // Add small random noise (imperfect adjustment)
        new_offset += noise_dist(rng);

        spdlog::debug("[MockScrewsTilt] {} adjustment: {:.3f}mm -> {:.3f}mm ({}% correction)",
                      screw.name, screw.current_offset, new_offset,
                      static_cast<int>(correction_factor * 100));

        screw.current_offset = new_offset;
    }
}

float MockScrewsTiltState::spread_mm() const {
    if (screws_.empty()) {
        return 0.0f;
    }
    float highest = screws_.front().current_offset;
    float lowest = screws_.front().current_offset;
    for (const auto& screw : screws_) {
        highest = std::max(highest, screw.current_offset);
        lowest = std::min(lowest, screw.current_offset);
    }
    return highest - lowest;
}

bool MockScrewsTiltState::is_level(float tolerance_mm) const {
    // Corner-to-corner spread, not each screw's distance from the base: the base
    // can sit mid-range, which is exactly how a tilted bed read as level in
    // prestonbrown/helixscreen#1225. Mirrors evaluate_screw_level().
    return spread_mm() <= tolerance_mm;
}

std::string MockScrewsTiltState::diff_to_adjustment(float diff_mm) {
    // Klipper (screws_tilt_adjust.py) computes diff = z_base - z and emits CW
    // for a positive diff on a CW-M* thread, CCW for a negative one. The mock
    // must match: emitting the opposite sign is what made --test unable to
    // reproduce the level-verdict bug (prestonbrown/helixscreen#1225).
    const float mm_per_turn = screw_thread_pitch_mm(mock_internal::MOCK_SCREW_THREAD);

    // Klipper zeroes the adjustment inside a 1 micron deadband *before* dividing
    // by the pitch, so probe noise never prints as a fraction of a turn.
    const float adjust = (std::abs(diff_mm) < 0.001f) ? 0.0f : diff_mm / mm_per_turn;

    // Direction is taken from the zeroed value, so a deadbanded diff always
    // reads CW 00:00 regardless of which side of zero it landed on.
    const char* direction = (adjust >= 0.0f) ? "CW" : "CCW";

    // math.trunc() + round(decimal * 60) with NO carry into full_turns: real
    // Klipper genuinely emits "00:60" for a near-full turn, and the mock has to
    // reproduce that so the parser is tested against what printers actually say.
    const int full_turns = static_cast<int>(adjust);
    const float decimal_part = adjust - static_cast<float>(full_turns);
    const int minutes = static_cast<int>(std::lround(decimal_part * 60.0f));

    // Format as "CW 01:15" or "CCW 00:30"
    char buf[24];
    snprintf(buf, sizeof(buf), "%s %02d:%02d", direction, std::abs(full_turns), std::abs(minutes));
    return std::string(buf);
}

// ============================================================================
// MoonrakerAdvancedAPIMock - Calibration Overrides
// ============================================================================

void MoonrakerAdvancedAPIMock::calculate_screws_tilt(ScrewTiltCallback on_success,
                                                     ErrorCallback /*on_error*/) {
    spdlog::info("[MoonrakerAdvancedAPIMock] calculate_screws_tilt called (probe #{})",
                 mock_bed_state_.get_probe_count() + 1);

    // Feed the simulated console output through the SAME parser the live
    // collector uses, so --test exercises the line parser and the printer-database
    // screws_tilt_direction override instead of bypassing both.
    std::vector<ScrewTiltResult> results;
    for (const auto& line : mock_bed_state_.probe_lines()) {
        ScrewTiltResult result;
        if (helix::parse_screws_tilt_line(line, result)) {
            results.push_back(std::move(result));
        }
    }

    // After showing results, simulate user making adjustments
    mock_bed_state_.simulate_user_adjustments();

    if (on_success) {
        on_success(results);
    }
}

void MoonrakerAdvancedAPIMock::reset_mock_bed_state() {
    mock_bed_state_.reset();
    spdlog::info("[MoonrakerAdvancedAPIMock] Mock bed state reset");
}

void MoonrakerAdvancedAPIMock::start_bed_mesh_calibrate(
    const BedMeshCommand& command, BedMeshProgressCallback on_progress, SuccessCallback on_complete,
    ErrorCallback /*on_error*/, int /*expected_probes*/, int /*probe_samples*/) {
    spdlog::info("[MoonrakerAdvancedAPIMock] start_bed_mesh_calibrate('{}') - simulating probe "
                 "sequence",
                 command.script);

    // Context struct to track state across timer callbacks
    struct ProbeSimContext {
        MoonrakerAdvancedAPIMock* advanced;
        BedMeshProgressCallback on_progress;
        SuccessCallback on_complete;
        std::string script;
        int current = 0;
        int total = 49; // 7x7 mesh = 49 probe points
    };

    auto* ctx =
        new ProbeSimContext{this, std::move(on_progress), std::move(on_complete), command.script};

    // Timer callback - advances probe simulation one step at a time
    auto timer_cb = [](lv_timer_t* t) {
        auto* c = static_cast<ProbeSimContext*>(lv_timer_get_user_data(t));
        c->current++;

        if (c->current <= c->total) {
            // Report progress
            spdlog::debug("[MoonrakerAdvancedAPIMock] Probe {}/{}", c->current, c->total);
            if (c->on_progress) {
                c->on_progress(c->current, c->total);
            }
        }

        if (c->current >= c->total) {
            // Simulation complete - regenerate mesh with new random data
            spdlog::info("[MoonrakerAdvancedAPIMock] Probe simulation complete, regenerating mesh");
            lv_timer_delete(t);

            // The client mock regenerates the mesh and stores it in whatever
            // profile the command's PROFILE= names, as Klipper does.
            c->advanced->api_.execute_gcode(
                c->script,
                [c]() {
                    spdlog::debug("[MoonrakerAdvancedAPIMock] Mesh regenerated");
                    if (c->on_complete) {
                        c->on_complete();
                    }
                    delete c;
                },
                [c](const MoonrakerError& err) {
                    spdlog::error("[MoonrakerAdvancedAPIMock] Mesh regen failed: {}", err.message);
                    if (c->on_complete) {
                        c->on_complete(); // Still complete the UI flow
                    }
                    delete c;
                });
        }
    };

    // Create timer - 50ms between each probe point (~2.5 seconds total for 49 points)
    lv_timer_t* timer = lv_timer_create(timer_cb, 50, ctx);
    lv_timer_set_repeat_count(timer, ctx->total + 1); // +1 for final completion check
}

// ============================================================================
// MoonrakerRestAPIMock - REST Endpoint Methods
// ============================================================================

void MoonrakerRestAPIMock::call_rest_get(const std::string& endpoint, RestCallback on_complete) {
    spdlog::debug("[MoonrakerAPIMock] REST GET: {}", endpoint);

    // Test-configured per-endpoint override wins over the built-in canned
    // responses so tests can drive 404 / failure paths and custom payloads.
    if (auto it = get_responses_.find(endpoint); it != get_responses_.end()) {
        if (on_complete) {
            on_complete(it->second);
        }
        return;
    }

    RestResponse resp;
    resp.success = true;
    resp.status_code = 200;

    // Return mock responses for known ACE endpoints (via ValgACE Moonraker plugin)
    if (endpoint == "/server/ace/info") {
        resp.data = {
            {"result", {{"model", "ACE Pro"}, {"version", "1.0.0-mock"}, {"slot_count", 4}}}};
    } else if (endpoint == "/server/ace/status") {
        resp.data = {{"result",
                      {{"loaded_slot", -1},
                       {"action", "idle"},
                       {"dryer",
                        {{"active", false},
                         {"current_temp", 25.0},
                         {"target_temp", 0.0},
                         {"remaining_minutes", 0},
                         {"duration_minutes", 0}}}}}};
    } else if (endpoint == "/server/ace/slots") {
        resp.data = {{"result",
                      {{"slots",
                        {{{"status", "available"},
                          {"color", "#FF0000"},
                          {"material", "PLA"},
                          {"temp_min", 190},
                          {"temp_max", 220}},
                         {{"status", "available"},
                          {"color", "#00FF00"},
                          {"material", "PETG"},
                          {"temp_min", 220},
                          {"temp_max", 250}},
                         {{"status", "empty"},
                          {"color", "#000000"},
                          {"material", ""},
                          {"temp_min", 0},
                          {"temp_max", 0}},
                         {{"status", "available"},
                          {"color", "#0000FF"},
                          {"material", "ABS"},
                          {"temp_min", 240},
                          {"temp_max", 270}}}}}}};
    } else {
        // Unknown endpoint - return generic success with empty result
        resp.data = {{"result", nlohmann::json::object()}};
        spdlog::debug("[MoonrakerAPIMock] Unknown REST endpoint: {}", endpoint);
    }

    if (on_complete) {
        on_complete(resp);
    }
}

void MoonrakerRestAPIMock::call_rest_post(const std::string& endpoint, const nlohmann::json& params,
                                          RestCallback on_complete) {
    spdlog::debug("[MoonrakerAPIMock] REST POST: {} ({} bytes)", endpoint, params.dump().size());

    // Record for test spies before invoking the callback so even tests that
    // assert from inside on_complete (synchronous mock) see the entry.
    post_history_.push_back({endpoint, params});

    // Honor a queued canned response (drives 404 / "state":"error" branches);
    // otherwise default to a generic success.
    RestResponse resp;
    auto it = post_responses_.find(endpoint);
    if (it != post_responses_.end()) {
        resp = it->second;
    } else {
        resp.success = true;
        resp.status_code = 200;
        resp.data = {{"result", "ok"}};
    }

    if (on_complete) {
        on_complete(resp);
    }
}

// ============================================================================
// MoonrakerTimelapseAPIMock Implementation
// ============================================================================

MoonrakerTimelapseAPIMock::MoonrakerTimelapseAPIMock(MoonrakerClient& client,
                                                     const std::string& http_base_url)
    : MoonrakerTimelapseAPI(client, http_base_url) {
    // Frame count and capture info are set in get_last_frame_info() which the
    // overlay calls on_activate(). Can't set here — subjects not yet initialized.
}

void MoonrakerTimelapseAPIMock::render_timelapse(SuccessCallback on_success,
                                                 ErrorCallback /*on_error*/) {
    spdlog::info("[MoonrakerAPIMock] render_timelapse (mock) - simulating render progress");

    // Guard against double-click — if already rendering, ignore
    auto* status_subj = helix::TimelapseState::instance().get_render_status_subject();
    if (status_subj) {
        const char* status = lv_subject_get_string(status_subj);
        if (status && std::strcmp(status, "rendering") == 0) {
            spdlog::debug("[MoonrakerAPIMock] Render already in progress, ignoring");
            return;
        }
    }

    // Context struct to track state across timer callbacks
    struct RenderSimContext {
        SuccessCallback on_complete;
        int current_progress = 0;
    };

    auto* ctx = new RenderSimContext{std::move(on_success)};

    // Timer callback - advances render progress in 5% increments
    auto timer_cb = [](lv_timer_t* t) {
        auto* c = static_cast<RenderSimContext*>(lv_timer_get_user_data(t));
        c->current_progress += 5;

        if (c->current_progress < 100) {
            // Send progress event
            nlohmann::json event;
            event["action"] = "render";
            event["status"] = "running";
            event["progress"] = c->current_progress;
            helix::TimelapseState::instance().handle_timelapse_event(event);
        }

        if (c->current_progress >= 100) {
            // Send success event with a mock filename
            nlohmann::json event;
            event["action"] = "render";
            event["status"] = "success";
            event["progress"] = 100;
            event["filename"] = "mock_render_timelapse.mp4";
            helix::TimelapseState::instance().handle_timelapse_event(event);

            if (c->on_complete) {
                c->on_complete();
            }
            delete c;
            lv_timer_delete(t);
        }
    };

    // 20 steps of 5% = 100%, at 150ms each = ~3 seconds total
    lv_timer_create(timer_cb, 150, ctx);
}

void MoonrakerTimelapseAPIMock::save_timelapse_frames(SuccessCallback on_success,
                                                      ErrorCallback /*on_error*/) {
    spdlog::debug("[MoonrakerAPIMock] save_timelapse_frames (mock)");
    if (on_success)
        on_success();
}

void MoonrakerTimelapseAPIMock::get_last_frame_info(
    std::function<void(const LastFrameInfo&)> on_success, ErrorCallback /*on_error*/) {
    spdlog::debug("[MoonrakerAPIMock] get_last_frame_info (mock)");

    // Set frame count and capture info subjects directly (we're on the UI thread)
    auto& tl = helix::TimelapseState::instance();
    lv_subject_set_int(tl.get_frame_count_subject(), 42);
    lv_subject_copy_string(tl.get_capture_info_subject(), "3DBenchy.gcode \xC2\xB7 Mar 10, 14:32");

    if (on_success) {
        LastFrameInfo info;
        info.frame_count = 42;
        on_success(info);
    }
}
