// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_api_mock.h"

#include "ui_update_queue.h"

#include "../tests/mocks/mock_printer_state.h"
#include "gcode_parser.h"
#include "moonraker_client_mock_internal.h"
#include "power_device_state.h"
#include "runtime_config.h"
#include "screws_tilt_parser.h"
#include "sensor_state.h"
#include "timelapse_state.h"

#include <spdlog/spdlog.h>

// Alias for cleaner code - use shared constant from RuntimeConfig
#define TEST_GCODE_DIR RuntimeConfig::TEST_GCODE_DIR

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
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
    rest_api_ = std::make_unique<MoonrakerRestAPIMock>(client, get_http_base_url());
    spoolman_api_ = std::make_unique<MoonrakerSpoolmanAPIMock>(client);
    timelapse_api_ = std::make_unique<MoonrakerTimelapseAPIMock>(client, get_http_base_url());
}

MoonrakerAdvancedAPIMock& MoonrakerAPIMock::advanced_mock() {
    return static_cast<MoonrakerAdvancedAPIMock&>(*advanced_api_);
}

MoonrakerFileTransferAPIMock& MoonrakerAPIMock::transfers_mock() {
    return static_cast<MoonrakerFileTransferAPIMock&>(*file_transfer_api_);
}

MoonrakerRestAPIMock& MoonrakerAPIMock::rest_mock() {
    return static_cast<MoonrakerRestAPIMock&>(*rest_api_);
}

MoonrakerSpoolmanAPIMock& MoonrakerAPIMock::spoolman_mock() {
    return static_cast<MoonrakerSpoolmanAPIMock&>(*spoolman_api_);
}

MoonrakerTimelapseAPIMock& MoonrakerAPIMock::timelapse_mock() {
    return static_cast<MoonrakerTimelapseAPIMock&>(*timelapse_api_);
}

// ============================================================================
// MoonrakerSpoolmanAPIMock Implementation
// ============================================================================

MoonrakerSpoolmanAPIMock::MoonrakerSpoolmanAPIMock(MoonrakerClient& client)
    : MoonrakerSpoolmanAPI(client) {
    init_mock_spools();
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

void MoonrakerAPIMock::register_method_callback(const std::string& /*method*/,
                                                const std::string& /*name*/,
                                                std::function<void(const json&)> /*callback*/) {
    // No-op in mock
}

bool MoonrakerAPIMock::unregister_method_callback(const std::string& /*method*/,
                                                  const std::string& /*name*/) {
    return true;
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

void MoonrakerAPIMock::database_get_item(const std::string& namespace_name, const std::string& key,
                                         std::function<void(const json&)> on_success,
                                         ErrorCallback on_error) {
    std::string db_key = namespace_name + ":" + key;
    auto it = mock_db_.find(db_key);
    if (it != mock_db_.end()) {
        if (on_success) {
            on_success(it->second);
        }
    } else {
        if (on_error) {
            MoonrakerError err = MoonrakerError::unknown("Key not found in mock DB");
            on_error(err);
        }
    }
}

void MoonrakerAPIMock::mock_set_db_value(const std::string& namespace_name, const std::string& key,
                                         const nlohmann::json& value) {
    mock_db_[namespace_name + ":" + key] = value;
}

nlohmann::json MoonrakerAPIMock::mock_get_db_value(const std::string& namespace_name,
                                                   const std::string& key) const {
    auto it = mock_db_.find(namespace_name + ":" + key);
    if (it == mock_db_.end())
        return nlohmann::json();
    return it->second;
}

void MoonrakerAPIMock::mock_reject_next_db_post() {
    next_db_post_rejection_ = MoonrakerError::unknown("Mock: database_post_item rejected");
}

void MoonrakerAPIMock::mock_reject_next_db_post(MoonrakerError err) {
    next_db_post_rejection_ = std::move(err);
}

void MoonrakerAPIMock::mock_reject_next_db_delete() {
    next_db_delete_rejection_ = MoonrakerError::unknown("Mock: database_delete_item rejected");
}

void MoonrakerAPIMock::mock_reject_next_db_delete(MoonrakerError err) {
    next_db_delete_rejection_ = std::move(err);
}

void MoonrakerAPIMock::mock_reject_next_db_get() {
    next_db_get_rejection_ = MoonrakerError::unknown("Mock: database_get_namespace rejected");
}

void MoonrakerAPIMock::mock_reject_next_db_get(MoonrakerError err) {
    next_db_get_rejection_ = std::move(err);
}

void MoonrakerAPIMock::mock_defer_next_db_post() {
    defer_next_db_post_ = true;
}

void MoonrakerAPIMock::fire_deferred_db_post_success() {
    // No-op if nothing was captured. This is important for the
    // destruction-before-fire lifetime tests: a test can safely call this in
    // cleanup without caring whether a previous test consumed it.
    if (!deferred_db_post_.has_value())
        return;
    auto captured = std::move(*deferred_db_post_);
    deferred_db_post_.reset();
    // Apply the deferred DB write now that the "server" has acknowledged.
    mock_db_[captured.namespace_name + ":" + captured.key] = captured.value;
    if (captured.on_success)
        captured.on_success();
}

void MoonrakerAPIMock::fire_deferred_db_post_error(MoonrakerError err) {
    if (!deferred_db_post_.has_value())
        return;
    auto captured = std::move(*deferred_db_post_);
    deferred_db_post_.reset();
    if (captured.on_error)
        captured.on_error(err);
}

void MoonrakerAPIMock::mock_defer_next_db_delete() {
    defer_next_db_delete_ = true;
}

void MoonrakerAPIMock::fire_deferred_db_delete_success() {
    if (!deferred_db_delete_.has_value())
        return;
    auto captured = std::move(*deferred_db_delete_);
    deferred_db_delete_.reset();
    // Apply the deferred DB erase now that the "server" has acknowledged.
    mock_db_.erase(captured.namespace_name + ":" + captured.key);
    if (captured.on_success)
        captured.on_success();
}

void MoonrakerAPIMock::fire_deferred_db_delete_error(MoonrakerError err) {
    if (!deferred_db_delete_.has_value())
        return;
    auto captured = std::move(*deferred_db_delete_);
    deferred_db_delete_.reset();
    if (captured.on_error)
        captured.on_error(err);
}

void MoonrakerAPIMock::mock_defer_next_db_get() {
    defer_next_db_get_ = true;
}

void MoonrakerAPIMock::fire_deferred_db_get_success(const nlohmann::json& value) {
    if (!deferred_db_get_.has_value())
        return;
    auto captured = std::move(*deferred_db_get_);
    deferred_db_get_.reset();
    if (captured.on_success)
        captured.on_success(value);
}

void MoonrakerAPIMock::fire_deferred_db_get_error(MoonrakerError err) {
    if (!deferred_db_get_.has_value())
        return;
    auto captured = std::move(*deferred_db_get_);
    deferred_db_get_.reset();
    if (captured.on_error)
        captured.on_error(err);
}

void MoonrakerAPIMock::set_database_empty(const std::string& namespace_name,
                                          const std::string& key) {
    mock_db_.erase(namespace_name + ":" + key);
}

void MoonrakerAPIMock::database_post_item(const std::string& namespace_name, const std::string& key,
                                          const json& value, std::function<void()> on_success,
                                          ErrorCallback on_error) {
    ++db_post_count_;
    if (next_db_post_rejection_.has_value()) {
        MoonrakerError err = std::move(*next_db_post_rejection_);
        next_db_post_rejection_.reset();
        if (on_error) {
            on_error(err);
        }
        return;
    }
    if (defer_next_db_post_) {
        // Capture for later fire_deferred_db_post_*. Importantly, we do NOT
        // write to the DB yet — the fire_*_success path applies the write so
        // behavior matches a real server that only mutates state after ACK.
        defer_next_db_post_ = false;
        DeferredDbPost captured;
        captured.on_success = std::move(on_success);
        captured.on_error = std::move(on_error);
        captured.namespace_name = namespace_name;
        captured.key = key;
        captured.value = value;
        deferred_db_post_ = std::move(captured);
        return;
    }
    mock_db_[namespace_name + ":" + key] = value;
    if (on_success) {
        on_success();
    }
}

void MoonrakerAPIMock::database_get_namespace(const std::string& namespace_name,
                                              std::function<void(const json&)> on_success,
                                              ErrorCallback on_error) {
    if (next_db_get_rejection_.has_value()) {
        MoonrakerError err = std::move(*next_db_get_rejection_);
        next_db_get_rejection_.reset();
        if (on_error) {
            on_error(err);
        }
        return;
    }
    if (defer_next_db_get_) {
        defer_next_db_get_ = false;
        DeferredDbGet captured;
        captured.on_success = std::move(on_success);
        captured.on_error = std::move(on_error);
        captured.namespace_name = namespace_name;
        deferred_db_get_ = std::move(captured);
        return;
    }
    // Collect all entries whose mock key starts with "<namespace_name>:" into
    // a single JSON object keyed by the portion after the colon.
    nlohmann::json result = nlohmann::json::object();
    const std::string prefix = namespace_name + ":";
    for (const auto& [db_key, value] : mock_db_) {
        if (db_key.rfind(prefix, 0) == 0) {
            result[db_key.substr(prefix.size())] = value;
        }
    }
    if (on_success) {
        on_success(result);
    }
}

void MoonrakerAPIMock::database_delete_item(const std::string& namespace_name,
                                            const std::string& key,
                                            std::function<void()> on_success,
                                            ErrorCallback on_error) {
    ++db_delete_count_;
    if (next_db_delete_rejection_.has_value()) {
        MoonrakerError err = std::move(*next_db_delete_rejection_);
        next_db_delete_rejection_.reset();
        // Mirror MoonrakerAPI::database_delete_item's missing-key normalization
        // so the mock faithfully simulates the real API's contract. Tests that
        // inject a 404 or "not found" error through mock_reject_next_db_delete
        // verify callers see on_success — exactly as they would against a real
        // Moonraker instance.
        const bool missing_key =
            err.code == 404 || err.message.find("not found") != std::string::npos;
        if (missing_key) {
            if (on_success) {
                on_success();
            }
            return;
        }
        if (on_error) {
            on_error(err);
        }
        return;
    }
    if (defer_next_db_delete_) {
        // Capture for later fire_deferred_db_delete_*. We do NOT erase from the
        // DB yet — fire_*_success applies the erase so behavior matches a real
        // server that only mutates state after ACK.
        defer_next_db_delete_ = false;
        DeferredDbPost captured; // reused shape (success/error identical)
        captured.on_success = std::move(on_success);
        captured.on_error = std::move(on_error);
        captured.namespace_name = namespace_name;
        captured.key = key;
        deferred_db_delete_ = std::move(captured);
        return;
    }
    // Absent keys are a silent success (matches Moonraker's semantics after the
    // real impl maps its "key not found" error to success).
    mock_db_.erase(namespace_name + ":" + key);
    if (on_success) {
        on_success();
    }
}

// ============================================================================
// Helix Plugin Method Overrides (mock)
// ============================================================================

void MoonrakerAPIMock::get_phase_tracking_status(std::function<void(bool)> on_success,
                                                 ErrorCallback /*on_error*/) {
    if (on_success) {
        on_success(false);
    }
}

void MoonrakerAPIMock::set_phase_tracking_enabled(bool enabled,
                                                  std::function<void(bool)> on_success,
                                                  ErrorCallback /*on_error*/) {
    spdlog::debug("[MoonrakerAPIMock] set_phase_tracking_enabled({})", enabled);
    if (on_success) {
        on_success(enabled);
    }
}

std::string MoonrakerFileTransferAPIMock::find_test_file(const std::string& filename) const {
    namespace fs = std::filesystem;

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

void MoonrakerFileTransferAPIMock::download_file(const std::string& root, const std::string& path,
                                                 StringCallback on_success,
                                                 ErrorCallback on_error) {
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
                                                         ErrorCallback on_error) {
    // Strip any leading directory components to get just the filename
    std::string filename = path;
    size_t last_slash = path.rfind('/');
    if (last_slash != std::string::npos) {
        filename = path.substr(last_slash + 1);
    }

    spdlog::debug("[MoonrakerAPIMock] download_file_partial: root='{}', path='{}', max_bytes={}",
                  root, path, max_bytes);

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

void MoonrakerFileTransferAPIMock::upload_file(const std::string& root, const std::string& path,
                                               const std::string& content,
                                               SuccessCallback on_success, ErrorCallback on_error) {
    (void)on_error; // Unused - mock always succeeds

    spdlog::info("[MoonrakerAPIMock] Mock upload_file: root='{}', path='{}', size={} bytes", root,
                 path, content.size());

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
    static const bool remote_thumbs = [] {
        const char* v = std::getenv("HELIX_MOCK_REMOTE_THUMBS");
        return v && v[0] && std::string(v) != "0";
    }();
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
            // Extract thumbnails from the G-code file
            auto thumbnails = helix::gcode::extract_thumbnails(gcode_path);
            if (!thumbnails.empty()) {
                // Find the largest thumbnail (best quality)
                const helix::gcode::GCodeThumbnail* best = &thumbnails[0];
                for (const auto& thumb : thumbnails) {
                    if (thumb.pixel_count() > best->pixel_count()) {
                        best = &thumb;
                    }
                }

                // Write the thumbnail to the cache path
                std::ofstream file(cache_path, std::ios::binary);
                if (file) {
                    file.write(reinterpret_cast<const char*>(best->png_data.data()),
                               static_cast<std::streamsize>(best->png_data.size()));
                    file.close();

                    spdlog::info(
                        "[MoonrakerAPIMock] Extracted thumbnail {}x{} ({} bytes) from {} -> {}",
                        best->width, best->height, best->png_data.size(), gcode_filename,
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
    if (std::getenv("MOCK_EMPTY_POWER")) {
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

void MoonrakerRestAPIMock::wled_get_strips(RestCallback on_success, ErrorCallback /*on_error*/) {
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

void MoonrakerRestAPIMock::get_server_config(RestCallback on_success, ErrorCallback /*on_error*/) {
    spdlog::info("[MoonrakerAPIMock] get_server_config");

    if (on_success) {
        RestResponse resp;
        resp.success = true;
        resp.status_code = 200;
        resp.data = {
            {"result",
             {{"config",
               {{"wled printer_led",
                 {{"type", "http"}, {"address", "192.168.1.50"}, {"initial_preset", -1}}},
                {"wled enclosure_led",
                 {{"type", "http"}, {"address", "192.168.1.51"}, {"initial_preset", -1}}}}}}}};
        on_success(resp);
    }
}

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

void MoonrakerAdvancedAPIMock::start_bed_mesh_calibrate(BedMeshProgressCallback on_progress,
                                                        SuccessCallback on_complete,
                                                        ErrorCallback /*on_error*/,
                                                        int /*expected_probes*/,
                                                        int /*probe_samples*/) {
    spdlog::info(
        "[MoonrakerAdvancedAPIMock] start_bed_mesh_calibrate() - simulating probe sequence");

    // Context struct to track state across timer callbacks
    struct ProbeSimContext {
        MoonrakerAdvancedAPIMock* advanced;
        BedMeshProgressCallback on_progress;
        SuccessCallback on_complete;
        int current = 0;
        int total = 49; // 7x7 mesh = 49 probe points
    };

    auto* ctx = new ProbeSimContext{this, std::move(on_progress), std::move(on_complete)};

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

            // Send BED_MESH_CALIBRATE to client mock to regenerate mesh data
            // Match real API: no PROFILE= parameter, mesh goes to "default" profile
            c->advanced->api_.execute_gcode(
                "BED_MESH_CALIBRATE",
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
// MoonrakerSpoolmanAPIMock - Spoolman Override
// ============================================================================

void MoonrakerSpoolmanAPIMock::init_mock_spools() {
    // Create a realistic mock spool inventory
    mock_spools_.clear();

    // Spool 1: Polymaker PLA - Jet Black (active, 85% remaining)
    SpoolInfo spool1;
    spool1.id = 1;
    spool1.vendor = "Polymaker";
    spool1.material = "PLA";
    spool1.filament_name = "Jet Black";
    spool1.color_hex = "1A1A2E";
    spool1.remaining_weight_g = 850.0;
    spool1.initial_weight_g = 1000.0;
    spool1.remaining_length_m = 290.0;
    spool1.spool_weight_g = 140.0;
    spool1.nozzle_temp_recommended = 210;
    spool1.bed_temp_recommended = 60;
    spool1.is_active = true;
    mock_spools_.push_back(spool1);

    // Spool 2: eSUN Silk PLA - Silk Blue (75% remaining)
    SpoolInfo spool2;
    spool2.id = 2;
    spool2.vendor = "eSUN";
    spool2.material = "Silk PLA";
    spool2.filament_name = "Silk Blue";
    spool2.color_hex = "26DCD9";
    spool2.remaining_weight_g = 750.0;
    spool2.initial_weight_g = 1000.0;
    spool2.remaining_length_m = 258.0;
    spool2.spool_weight_g = 240.0;
    spool2.nozzle_temp_recommended = 210;
    spool2.bed_temp_recommended = 50;
    spool2.is_active = false;
    mock_spools_.push_back(spool2);

    // Spool 3: Elegoo ASA - Pop Blue (50% remaining)
    SpoolInfo spool3;
    spool3.id = 3;
    spool3.vendor = "Elegoo";
    spool3.material = "ASA";
    spool3.filament_name = "Pop Blue";
    spool3.color_hex = "00AEFF";
    spool3.remaining_weight_g = 500.0;
    spool3.initial_weight_g = 1000.0;
    spool3.remaining_length_m = 185.0;
    spool3.spool_weight_g = 170.0;
    spool3.nozzle_temp_recommended = 260;
    spool3.bed_temp_recommended = 100;
    spool3.is_active = false;
    mock_spools_.push_back(spool3);

    // Spool 4: Flashforge ABS - Fire Engine Red (LOW: 10% remaining)
    SpoolInfo spool4;
    spool4.id = 4;
    spool4.vendor = "Flashforge";
    spool4.material = "ABS";
    spool4.filament_name = "Fire Engine Red";
    spool4.color_hex = "D20000";
    spool4.remaining_weight_g = 100.0;
    spool4.initial_weight_g = 1000.0;
    spool4.remaining_length_m = 39.0;
    spool4.spool_weight_g = 160.0;
    spool4.nozzle_temp_recommended = 260;
    spool4.bed_temp_recommended = 100;
    spool4.is_active = false;
    mock_spools_.push_back(spool4);

    // Spool 5: Kingroon PETG - Signal Yellow (NEW: 100% remaining)
    SpoolInfo spool5;
    spool5.id = 5;
    spool5.vendor = "Kingroon";
    spool5.material = "PETG";
    spool5.filament_name = "Signal Yellow";
    spool5.color_hex = "F4E111";
    spool5.remaining_weight_g = 1000.0;
    spool5.initial_weight_g = 1000.0;
    spool5.remaining_length_m = 333.0;
    spool5.spool_weight_g = 155.0;
    spool5.nozzle_temp_recommended = 235;
    spool5.bed_temp_recommended = 70;
    spool5.is_active = false;
    mock_spools_.push_back(spool5);

    // Spool 6: Overture TPU - Clear (60% remaining)
    SpoolInfo spool6;
    spool6.id = 6;
    spool6.vendor = "Overture";
    spool6.material = "TPU";
    spool6.filament_name = "Clear";
    spool6.color_hex = "E8E8E8";
    spool6.remaining_weight_g = 600.0;
    spool6.initial_weight_g = 1000.0;
    spool6.remaining_length_m = 198.0;
    spool6.spool_weight_g = 230.0;
    spool6.nozzle_temp_recommended = 220;
    spool6.bed_temp_recommended = 50;
    spool6.is_active = false;
    mock_spools_.push_back(spool6);

    // === Additional spools from real Spoolman inventory for realistic testing ===

    // Spool 7: Bambu Lab ASA - Gray (NEW: 100%)
    SpoolInfo spool7;
    spool7.id = 7;
    spool7.vendor = "Bambu Lab";
    spool7.material = "ASA";
    spool7.filament_name = "Gray ASA";
    spool7.color_hex = "8A949E";
    spool7.remaining_weight_g = 1000.0;
    spool7.initial_weight_g = 1000.0;
    spool7.remaining_length_m = 370.0;
    spool7.spool_weight_g = 250.0;
    spool7.nozzle_temp_recommended = 250;
    spool7.bed_temp_recommended = 90;
    spool7.is_active = false;
    mock_spools_.push_back(spool7);

    // Spool 8: Polymaker PC - Grey (67% - Polycarbonate engineering material)
    SpoolInfo spool8;
    spool8.id = 8;
    spool8.vendor = "Polymaker";
    spool8.material = "PC";
    spool8.filament_name = "PolyMax PC Grey";
    spool8.color_hex = "A2AAAD";
    spool8.remaining_weight_g = 500.0;
    spool8.initial_weight_g = 750.0;
    spool8.remaining_length_m = 152.0;
    spool8.spool_weight_g = 125.0;
    spool8.nozzle_temp_recommended = 270;
    spool8.bed_temp_recommended = 100;
    spool8.is_active = false;
    mock_spools_.push_back(spool8);

    // Spool 9: Polymaker PA12-CF15 - Carbon Fiber Nylon (100% - HIGH TEMP)
    SpoolInfo spool9;
    spool9.id = 9;
    spool9.vendor = "Polymaker";
    spool9.material = "PA-CF";
    spool9.filament_name = "Fiberon PA12-CF15 Black";
    spool9.color_hex = "000000";
    spool9.remaining_weight_g = 500.0;
    spool9.initial_weight_g = 500.0;
    spool9.remaining_length_m = 170.0;
    spool9.spool_weight_g = 190.0;
    spool9.nozzle_temp_recommended = 290;
    spool9.bed_temp_recommended = 50;
    spool9.is_active = false;
    mock_spools_.push_back(spool9);

    // Spool 10: Tinmorry TPU - Blue (90% - Flexible)
    SpoolInfo spool10;
    spool10.id = 10;
    spool10.vendor = "Tinmorry";
    spool10.material = "TPU";
    spool10.filament_name = "Blue TPU";
    spool10.color_hex = "435FCC";
    spool10.remaining_weight_g = 900.0;
    spool10.initial_weight_g = 1000.0;
    spool10.remaining_length_m = 297.0;
    spool10.spool_weight_g = 200.0;
    spool10.nozzle_temp_recommended = 230;
    spool10.bed_temp_recommended = 50;
    spool10.is_active = false;
    mock_spools_.push_back(spool10);

    // Spool 11: eSUN ABS - Black (40%)
    SpoolInfo spool11;
    spool11.id = 11;
    spool11.vendor = "eSUN";
    spool11.material = "ABS";
    spool11.filament_name = "Black ABS+HS";
    spool11.color_hex = "000000";
    spool11.remaining_weight_g = 400.0;
    spool11.initial_weight_g = 1000.0;
    spool11.remaining_length_m = 148.0;
    spool11.spool_weight_g = 160.0;
    spool11.nozzle_temp_recommended = 260;
    spool11.bed_temp_recommended = 100;
    spool11.is_active = false;
    mock_spools_.push_back(spool11);

    // Spool 12: Flashforge ASA - Dark Green Sparkle (35%)
    SpoolInfo spool12;
    spool12.id = 12;
    spool12.vendor = "Flashforge";
    spool12.material = "ASA";
    spool12.filament_name = "Dark Green Sparkle ASA";
    spool12.color_hex = "276E27";
    spool12.remaining_weight_g = 350.0;
    spool12.initial_weight_g = 1000.0;
    spool12.remaining_length_m = 129.5;
    spool12.spool_weight_g = 175.0;
    spool12.nozzle_temp_recommended = 260;
    spool12.bed_temp_recommended = 100;
    spool12.is_active = false;
    mock_spools_.push_back(spool12);

    // Spool 13: Bambu Lab PETG - Translucent Green (100%)
    SpoolInfo spool13;
    spool13.id = 13;
    spool13.vendor = "Bambu Lab";
    spool13.material = "PETG";
    spool13.filament_name = "Translucent Green PETG";
    spool13.color_hex = "29A261";
    spool13.remaining_weight_g = 1000.0;
    spool13.initial_weight_g = 1000.0;
    spool13.remaining_length_m = 333.0;
    spool13.spool_weight_g = 250.0;
    spool13.nozzle_temp_recommended = 250;
    spool13.bed_temp_recommended = 70;
    spool13.is_active = false;
    mock_spools_.push_back(spool13);

    // Spool 14: Eryone Silk PLA - Gold/Silver/Copper (49% - tri-color)
    SpoolInfo spool14;
    spool14.id = 14;
    spool14.vendor = "Eryone";
    spool14.material = "Silk PLA";
    spool14.filament_name = "Gold/Silver/Copper Tri-Color";
    spool14.color_hex = "D4AF37";                          // Primary color (gold)
    spool14.multi_color_hexes = "#D4AF37,#C0C0C0,#B87333"; // Gold, Silver, Copper
    spool14.remaining_weight_g = 494.0;
    spool14.initial_weight_g = 1000.0;
    spool14.remaining_length_m = 170.0;
    spool14.spool_weight_g = 150.0;
    spool14.nozzle_temp_recommended = 220;
    spool14.bed_temp_recommended = 60;
    spool14.is_active = false;
    mock_spools_.push_back(spool14);

    // Spool 15: Bambu Lab PLA - Red (100%)
    SpoolInfo spool15;
    spool15.id = 15;
    spool15.vendor = "Bambu Lab";
    spool15.material = "PLA";
    spool15.filament_name = "Red PLA";
    spool15.color_hex = "C12E1F";
    spool15.remaining_weight_g = 1000.0;
    spool15.initial_weight_g = 1000.0;
    spool15.remaining_length_m = 340.0;
    spool15.spool_weight_g = 250.0;
    spool15.nozzle_temp_recommended = 220;
    spool15.bed_temp_recommended = 60;
    spool15.is_active = false;
    mock_spools_.push_back(spool15);

    // Spool 16: Polymaker ABS - Metallic Blue (17%)
    SpoolInfo spool16;
    spool16.id = 16;
    spool16.vendor = "Polymaker";
    spool16.material = "ABS";
    spool16.filament_name = "PolyLite ABS Metallic Blue";
    spool16.color_hex = "333C64";
    spool16.remaining_weight_g = 174.0;
    spool16.initial_weight_g = 1000.0;
    spool16.remaining_length_m = 64.0;
    spool16.spool_weight_g = 140.0;
    spool16.nozzle_temp_recommended = 260;
    spool16.bed_temp_recommended = 100;
    spool16.is_active = false;
    mock_spools_.push_back(spool16);

    // Spool 17: Sunlu PETG - Black (55%)
    SpoolInfo spool17;
    spool17.id = 17;
    spool17.vendor = "Sunlu";
    spool17.material = "PETG";
    spool17.filament_name = "Black PETG";
    spool17.color_hex = "000000";
    spool17.remaining_weight_g = 550.0;
    spool17.initial_weight_g = 1000.0;
    spool17.remaining_length_m = 183.0;
    spool17.spool_weight_g = 130.0;
    spool17.nozzle_temp_recommended = 255;
    spool17.bed_temp_recommended = 80;
    spool17.is_active = false;
    mock_spools_.push_back(spool17);

    // Spool 18: eSUN PLA+ - White (30%)
    SpoolInfo spool18;
    spool18.id = 18;
    spool18.vendor = "eSUN";
    spool18.material = "PLA+";
    spool18.filament_name = "PLA+ White";
    spool18.color_hex = "FFFFFF";
    spool18.remaining_weight_g = 300.0;
    spool18.initial_weight_g = 1000.0;
    spool18.remaining_length_m = 103.0;
    spool18.spool_weight_g = 170.0;
    spool18.nozzle_temp_recommended = 220;
    spool18.bed_temp_recommended = 60;
    spool18.is_active = false;
    mock_spools_.push_back(spool18);

    // Spool 19: TTYT3D Marble PLA - Black/White (85% - dual-color marble)
    SpoolInfo spool19;
    spool19.id = 19;
    spool19.vendor = "TTYT3D";
    spool19.material = "Marble PLA";
    spool19.filament_name = "Black/White Marble";
    spool19.color_hex = "202020";                  // Primary color (dark base)
    spool19.multi_color_hexes = "#202020,#F0F0F0"; // Black, White
    spool19.remaining_weight_g = 850.0;
    spool19.initial_weight_g = 1000.0;
    spool19.remaining_length_m = 292.0;
    spool19.spool_weight_g = 200.0;
    spool19.nozzle_temp_recommended = 210;
    spool19.bed_temp_recommended = 60;
    spool19.is_active = false;
    mock_spools_.push_back(spool19);

    spdlog::debug("[MoonrakerAPIMock] Initialized {} mock spools", mock_spools_.size());
}

void MoonrakerSpoolmanAPIMock::fail_spoolman_unavailable(const std::string& method,
                                                         const ErrorCallback& on_error) const {
    spdlog::debug("[MoonrakerAPIMock] {}() -> Spoolman disabled, failing like the real proxy",
                  method);
    if (on_error) {
        MoonrakerError err =
            MoonrakerError::json_rpc_error(method, "Spoolman component not available");
        on_error(err);
    }
}

void MoonrakerSpoolmanAPIMock::get_spoolman_status(std::function<void(bool, int)> on_success,
                                                   ErrorCallback /*on_error*/, bool /*silent*/) {
    spdlog::debug("[MoonrakerAPIMock] get_spoolman_status() -> connected={}, active={}",
                  mock_spoolman_enabled_, mock_active_spool_id_);

    if (on_success) {
        on_success(mock_spoolman_enabled_, mock_active_spool_id_);
    }
}

void MoonrakerSpoolmanAPIMock::get_spoolman_spools(SpoolListCallback on_success,
                                                   ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("get_spoolman_spools", on_error);
        return;
    }

    spdlog::debug("[MoonrakerAPIMock] get_spoolman_spools() -> {} spools", mock_spools_.size());

    if (on_success) {
        std::vector<SpoolInfo> sorted = mock_spools_;
        sort_spools_by_recency(sorted);
        on_success(sorted);
    }
}

void MoonrakerSpoolmanAPIMock::get_spoolman_spool(int spool_id, SpoolCallback on_success,
                                                  ErrorCallback on_error, bool /*silent*/) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("get_spoolman_spool", on_error);
        return;
    }

    // Search mock spools for the requested ID
    for (const auto& spool : mock_spools_) {
        if (spool.id == spool_id) {
            spdlog::trace("[MoonrakerAPIMock] get_spoolman_spool({}) -> {} {}", spool_id,
                          spool.vendor, spool.material);
            if (on_success) {
                on_success(spool);
            }
            return;
        }
    }

    // Spool not found - return empty optional
    spdlog::trace("[MoonrakerAPIMock] get_spoolman_spool({}) -> not found", spool_id);
    if (on_success) {
        on_success(std::nullopt);
    }
}

void MoonrakerSpoolmanAPIMock::set_active_spool(int spool_id, SuccessCallback on_success,
                                                ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("set_active_spool", on_error);
        return;
    }

    spdlog::info("[MoonrakerAPIMock] set_active_spool({}) - was {}", spool_id,
                 mock_active_spool_id_);

    // Update active spool state
    mock_active_spool_id_ = spool_id;

    // Update is_active flag on all spools
    for (auto& spool : mock_spools_) {
        spool.is_active = (spool.id == spool_id);
    }

    if (on_success) {
        on_success();
    }
}

void MoonrakerSpoolmanAPIMock::update_spoolman_spool_weight(int spool_id, double remaining_weight_g,
                                                            SuccessCallback on_success,
                                                            ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("update_spoolman_spool_weight", on_error);
        return;
    }

    spdlog::info("[MoonrakerAPIMock] update_spoolman_spool_weight({}, {:.1f}g)", spool_id,
                 remaining_weight_g);
    weight_updates.push_back({spool_id, remaining_weight_g});

    // Find and update the mock spool
    for (auto& spool : mock_spools_) {
        if (spool.id == spool_id) {
            spool.remaining_weight_g = remaining_weight_g;
            spdlog::debug("[MoonrakerAPIMock] Updated spool {} remaining weight to {:.1f}g",
                          spool_id, remaining_weight_g);
            break;
        }
    }

    if (on_success) {
        on_success();
    }
}

void MoonrakerSpoolmanAPIMock::update_spoolman_spool(int spool_id, const nlohmann::json& spool_data,
                                                     SuccessCallback on_success,
                                                     ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("update_spoolman_spool", on_error);
        return;
    }

    spdlog::info("[MoonrakerAPIMock] update_spoolman_spool({}, {} fields)", spool_id,
                 spool_data.size());
    spool_updates.push_back({spool_id, spool_data});

    for (auto& spool : mock_spools_) {
        if (spool.id == spool_id) {
            if (spool_data.contains("remaining_weight")) {
                spool.remaining_weight_g = spool_data["remaining_weight"].get<double>();
            }
            if (spool_data.contains("spool_weight")) {
                spool.spool_weight_g = spool_data["spool_weight"].get<double>();
            }
            if (spool_data.contains("price")) {
                spool.price = spool_data["price"].get<double>();
            }
            if (spool_data.contains("lot_nr")) {
                spool.lot_nr = spool_data["lot_nr"].get<std::string>();
            }
            if (spool_data.contains("comment")) {
                spool.comment = spool_data["comment"].get<std::string>();
            }
            if (spool_data.contains("location")) {
                spool.location = spool_data["location"].get<std::string>();
            }
            if (spool_data.contains("filament_id")) {
                spool.filament_id = spool_data["filament_id"].get<int>();
                // Re-resolve denormalized fields from the new filament so subsequent
                // GETs reflect the repoint (mirrors real Spoolman's response shape).
                for (const auto& f : mock_filaments_) {
                    if (f.id == spool.filament_id) {
                        spool.material = f.material;
                        spool.filament_name = f.filament_name;
                        spool.color_hex = f.color_hex;
                        spool.vendor_id = f.vendor_id;
                        spool.vendor = f.vendor_name;
                        break;
                    }
                }
            }
            spdlog::debug("[MoonrakerAPIMock] Updated spool {} with {} fields", spool_id,
                          spool_data.size());
            break;
        }
    }

    if (on_success) {
        on_success();
    }
}

void MoonrakerSpoolmanAPIMock::update_spoolman_filament(int filament_id,
                                                        const nlohmann::json& filament_data,
                                                        SuccessCallback on_success,
                                                        ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("update_spoolman_filament", on_error);
        return;
    }

    spdlog::info("[MoonrakerAPIMock] update_spoolman_filament({}, {} fields)", filament_id,
                 filament_data.size());
    filament_updates.push_back({filament_id, filament_data});
    if (on_success) {
        on_success();
    }
}

void MoonrakerSpoolmanAPIMock::update_spoolman_filament_color(int filament_id,
                                                              const std::string& color_hex,
                                                              SuccessCallback on_success,
                                                              ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("update_spoolman_filament_color", on_error);
        return;
    }

    spdlog::info("[MoonrakerAPIMock] update_spoolman_filament_color({}, {})", filament_id,
                 color_hex);

    // In a real Spoolman, filament_id refers to a shared filament definition.
    // For mock purposes, we'll update the color on all spools (simulating the shared nature).
    // In practice, you'd need to track filament_id separately from spool_id.
    spdlog::debug("[MoonrakerAPIMock] Mock: color update logged (filament {} -> {})", filament_id,
                  color_hex);

    if (on_success) {
        on_success();
    }
}

// ============================================================================
// MoonrakerSpoolmanAPIMock - Spoolman CRUD Methods
// ============================================================================

void MoonrakerSpoolmanAPIMock::get_spoolman_vendors(VendorListCallback on_success,
                                                    ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("get_spoolman_vendors", on_error);
        return;
    }

    spdlog::debug("[MoonrakerAPIMock] get_spoolman_vendors()");

    // Include explicitly seeded vendors first (stable IDs for tests).
    std::vector<VendorInfo> vendors = mock_vendors_;
    std::set<std::string> seen;
    std::set<int> seen_ids;
    int next_id = 1;
    for (const auto& v : vendors) {
        seen.insert(v.name);
        seen_ids.insert(v.id);
        if (v.id >= next_id) {
            next_id = v.id + 1;
        }
    }

    // Build vendor list from existing mock spools (deduplicate by vendor name)
    for (const auto& spool : mock_spools_) {
        if (!spool.vendor.empty() && seen.find(spool.vendor) == seen.end()) {
            seen.insert(spool.vendor);
            VendorInfo v;
            // Skip any IDs already claimed by seeded vendors.
            while (seen_ids.count(next_id)) {
                ++next_id;
            }
            v.id = next_id++;
            v.name = spool.vendor;
            vendors.push_back(v);
        }
    }

    spdlog::debug("[MoonrakerAPIMock] Returning {} vendors", vendors.size());
    if (on_success) {
        on_success(vendors);
    }
}

void MoonrakerSpoolmanAPIMock::get_spoolman_filaments(FilamentListCallback on_success,
                                                      ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("get_spoolman_filaments", on_error);
        return;
    }

    spdlog::debug("[MoonrakerAPIMock] get_spoolman_filaments()");

    // Build filament list from existing mock spools (deduplicate by vendor+material+color)
    std::vector<FilamentInfo> filaments;
    std::set<std::string> seen;
    std::set<int> seen_ids; // Track IDs from explicitly created filaments
    int next_id = 1;

    // Include explicitly created filaments first (they have stable IDs)
    for (const auto& mf : mock_filaments_) {
        filaments.push_back(mf);
        seen_ids.insert(mf.id);
        // Track by key to avoid duplicates with spool-synthesized entries
        std::string key = mf.vendor_name + "|" + mf.material + "|" + mf.filament_name;
        seen.insert(key);
        // Ensure auto-assigned IDs don't collide
        if (mf.id >= next_id) {
            next_id = mf.id + 1;
        }
    }

    // Synthesize filaments from spools (skip duplicates already covered above)
    for (const auto& spool : mock_spools_) {
        std::string key = spool.vendor + "|" + spool.material + "|" + spool.filament_name;
        if (seen.find(key) == seen.end()) {
            seen.insert(key);
            FilamentInfo f;
            f.id = next_id++;
            f.vendor_name = spool.vendor;
            f.material = spool.material;
            f.filament_name = spool.filament_name;
            f.color_hex = spool.color_hex;
            f.diameter = 1.75f;
            f.weight = static_cast<float>(spool.initial_weight_g);
            f.nozzle_temp_min = spool.nozzle_temp_min;
            f.nozzle_temp_max = spool.nozzle_temp_max;
            f.bed_temp_min = spool.bed_temp_min;
            f.bed_temp_max = spool.bed_temp_max;
            filaments.push_back(f);
        }
    }

    spdlog::debug("[MoonrakerAPIMock] Returning {} filaments ({} created + synthesized)",
                  filaments.size(), mock_filaments_.size());
    if (on_success) {
        on_success(filaments);
    }
}

void MoonrakerSpoolmanAPIMock::create_spoolman_vendor(const nlohmann::json& vendor_data,
                                                      VendorCreateCallback on_success,
                                                      ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("create_spoolman_vendor", on_error);
        return;
    }

    spdlog::info("[MoonrakerAPIMock] create_spoolman_vendor({})",
                 vendor_data.value("name", "unknown"));

    // Capture the POST payload for test inspection.
    created_vendors.push_back(vendor_data);

    VendorInfo vendor;
    vendor.id = next_created_vendor_id > 0
                    ? next_created_vendor_id
                    : static_cast<int>(mock_spools_.size()) + 100; // Avoid ID conflicts
    vendor.name = vendor_data.value("name", "");
    vendor.url = vendor_data.value("url", "");

    // Persist so subsequent get_spoolman_vendors() includes it.
    mock_vendors_.push_back(vendor);

    if (on_success) {
        on_success(vendor);
    }
}

void MoonrakerSpoolmanAPIMock::create_spoolman_filament(const nlohmann::json& filament_data,
                                                        FilamentCreateCallback on_success,
                                                        ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("create_spoolman_filament", on_error);
        return;
    }

    spdlog::info("[MoonrakerAPIMock] create_spoolman_filament({} {})",
                 filament_data.value("material", "?"), filament_data.value("name", "?"));

    // Capture the POST payload for test inspection.
    created_filaments.push_back(filament_data);

    FilamentInfo filament;
    filament.id = next_created_filament_id > 0 ? next_created_filament_id : next_filament_id_++;
    filament.material = filament_data.value("material", "");
    filament.filament_name = filament_data.value("name", "");
    filament.color_hex = filament_data.value("color_hex", "");
    filament.diameter = filament_data.value("diameter", 1.75f);
    filament.weight = filament_data.value("weight", 0.0f);
    filament.spool_weight = filament_data.value("spool_weight", 0.0f);

    if (filament_data.contains("vendor_id")) {
        filament.vendor_id = filament_data.value("vendor_id", 0);
        // Denormalize vendor_name from seeded or created vendors so GETs can serve it.
        for (const auto& v : mock_vendors_) {
            if (v.id == filament.vendor_id) {
                filament.vendor_name = v.name;
                break;
            }
        }
    }

    // Persist so subsequent get_spoolman_filaments() includes it
    mock_filaments_.push_back(filament);

    if (on_success) {
        on_success(filament);
    }
}

void MoonrakerSpoolmanAPIMock::create_spoolman_spool(const nlohmann::json& spool_data,
                                                     SpoolCreateCallback on_success,
                                                     ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("create_spoolman_spool", on_error);
        return;
    }

    spdlog::info("[MoonrakerAPIMock] create_spoolman_spool()");

    // Capture the POST payload for test inspection.
    created_spools.push_back(spool_data);

    // Create a new spool and add to the mock list
    SpoolInfo spool;
    spool.id = next_created_spool_id > 0 ? next_created_spool_id
                                         : static_cast<int>(mock_spools_.size()) + 1;
    spool.initial_weight_g = spool_data.value("initial_weight", 1000.0);
    spool.remaining_weight_g = spool_data.value("remaining_weight", spool.initial_weight_g);
    spool.spool_weight_g = spool_data.value("spool_weight", 0.0);
    if (spool_data.contains("filament_id")) {
        spool.filament_id = spool_data.value("filament_id", 0);
        // Resolve filament_id to its filament record (mirroring real Spoolman's GET
        // behavior where the spool response includes the linked filament's fields).
        for (const auto& f : mock_filaments_) {
            if (f.id == spool.filament_id) {
                spool.material = f.material;
                spool.filament_name = f.filament_name;
                spool.color_hex = f.color_hex;
                spool.vendor_id = f.vendor_id;
                spool.vendor = f.vendor_name;
                break;
            }
        }
    }

    // Persist optional spool-level string fields
    if (spool_data.contains("location") && spool_data["location"].is_string()) {
        spool.location = spool_data["location"].get<std::string>();
    }

    mock_spools_.push_back(spool);

    if (on_success) {
        on_success(spool);
    }
}

void MoonrakerSpoolmanAPIMock::delete_spoolman_spool(int spool_id, SuccessCallback on_success,
                                                     ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("delete_spoolman_spool", on_error);
        return;
    }

    spdlog::info("[MoonrakerAPIMock] delete_spoolman_spool({})", spool_id);

    // Remove from mock list
    auto it = std::remove_if(mock_spools_.begin(), mock_spools_.end(),
                             [spool_id](const SpoolInfo& s) { return s.id == spool_id; });
    if (it != mock_spools_.end()) {
        mock_spools_.erase(it, mock_spools_.end());
        spdlog::debug("[MoonrakerAPIMock] Spool {} removed from mock list", spool_id);
    }

    if (on_success) {
        on_success();
    }
}

void MoonrakerSpoolmanAPIMock::get_spoolman_external_vendors(VendorListCallback on_success,
                                                             ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("get_spoolman_external_vendors", on_error);
        return;
    }

    spdlog::debug("[MoonrakerAPIMock] get_spoolman_external_vendors()");

    std::vector<VendorInfo> vendors;

    VendorInfo v1;
    v1.id = 1;
    v1.name = "Hatchbox";
    v1.url = "https://www.hatchbox3d.com";
    vendors.push_back(v1);

    VendorInfo v2;
    v2.id = 2;
    v2.name = "Polymaker";
    v2.url = "https://www.polymaker.com";
    vendors.push_back(v2);

    VendorInfo v3;
    v3.id = 3;
    v3.name = "eSUN";
    v3.url = "https://www.esun3d.com";
    vendors.push_back(v3);

    VendorInfo v4;
    v4.id = 4;
    v4.name = "Prusament";
    v4.url = "https://www.prusa3d.com";
    vendors.push_back(v4);

    spdlog::debug("[MoonrakerAPIMock] Returning {} external vendors", vendors.size());
    if (on_success) {
        on_success(vendors);
    }
}

void MoonrakerSpoolmanAPIMock::get_spoolman_external_filaments(const std::string& vendor_name,
                                                               FilamentListCallback on_success,
                                                               ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("get_spoolman_external_filaments", on_error);
        return;
    }

    spdlog::debug("[MoonrakerAPIMock] get_spoolman_external_filaments(vendor={})", vendor_name);

    std::vector<FilamentInfo> filaments;

    FilamentInfo f1;
    f1.id = 1;
    f1.vendor_name = vendor_name;
    f1.material = "PLA";
    f1.filament_name = "Black";
    f1.color_hex = "000000";
    f1.diameter = 1.75f;
    f1.weight = 1000.0f;
    f1.nozzle_temp_min = 190;
    f1.nozzle_temp_max = 220;
    f1.bed_temp_min = 50;
    f1.bed_temp_max = 60;
    filaments.push_back(f1);

    FilamentInfo f2;
    f2.id = 2;
    f2.vendor_name = vendor_name;
    f2.material = "PLA";
    f2.filament_name = "White";
    f2.color_hex = "FFFFFF";
    f2.diameter = 1.75f;
    f2.weight = 1000.0f;
    f2.nozzle_temp_min = 190;
    f2.nozzle_temp_max = 220;
    f2.bed_temp_min = 50;
    f2.bed_temp_max = 60;
    filaments.push_back(f2);

    FilamentInfo f3;
    f3.id = 3;
    f3.vendor_name = vendor_name;
    f3.material = "PETG";
    f3.filament_name = "Blue";
    f3.color_hex = "0000FF";
    f3.diameter = 1.75f;
    f3.weight = 1000.0f;
    f3.nozzle_temp_min = 220;
    f3.nozzle_temp_max = 250;
    f3.bed_temp_min = 70;
    f3.bed_temp_max = 80;
    filaments.push_back(f3);

    spdlog::debug("[MoonrakerAPIMock] Returning {} external filaments for vendor '{}'",
                  filaments.size(), vendor_name);
    if (on_success) {
        on_success(filaments);
    }
}

void MoonrakerSpoolmanAPIMock::get_spoolman_filaments(int vendor_id,
                                                      FilamentListCallback on_success,
                                                      ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("get_spoolman_filaments", on_error);
        return;
    }

    spdlog::debug("[MoonrakerAPIMock] get_spoolman_filaments(vendor_id={})", vendor_id);

    std::vector<FilamentInfo> filaments;

    // Include explicitly-seeded filaments whose vendor_id matches.
    for (const auto& mf : mock_filaments_) {
        if (mf.vendor_id == vendor_id) {
            filaments.push_back(mf);
        }
    }

    // SpoolInfo doesn't have vendor_id so we can't filter spool-synthesized
    // filaments here. The wizard applies client-side vendor filtering after merge.
    int next_id = 1;
    for (const auto& spool : mock_spools_) {
        FilamentInfo f;
        f.id = next_id++;
        f.vendor_name = spool.vendor;
        f.material = spool.material;
        f.filament_name = spool.filament_name;
        f.color_hex = spool.color_hex;
        f.diameter = 1.75f;
        f.weight = static_cast<float>(spool.initial_weight_g);
        f.nozzle_temp_min = spool.nozzle_temp_min;
        f.nozzle_temp_max = spool.nozzle_temp_max;
        f.bed_temp_min = spool.bed_temp_min;
        f.bed_temp_max = spool.bed_temp_max;
        filaments.push_back(f);
    }

    spdlog::debug("[MoonrakerAPIMock] Returning {} filaments for vendor_id {}", filaments.size(),
                  vendor_id);
    if (on_success) {
        on_success(filaments);
    }
}

void MoonrakerSpoolmanAPIMock::delete_spoolman_vendor(int vendor_id, SuccessCallback on_success,
                                                      ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("delete_spoolman_vendor", on_error);
        return;
    }

    spdlog::info("[MoonrakerAPIMock] delete_spoolman_vendor({})", vendor_id);

    if (on_success) {
        on_success();
    }
}

void MoonrakerSpoolmanAPIMock::delete_spoolman_filament(int filament_id, SuccessCallback on_success,
                                                        ErrorCallback on_error) {
    if (!mock_spoolman_enabled_) {
        fail_spoolman_unavailable("delete_spoolman_filament", on_error);
        return;
    }

    spdlog::info("[MoonrakerAPIMock] delete_spoolman_filament({})", filament_id);

    if (on_success) {
        on_success();
    }
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
// MoonrakerSpoolmanAPIMock - Slot-Spool Mapping
// ============================================================================

void MoonrakerSpoolmanAPIMock::assign_spool_to_slot(int slot_index, int spool_id) {
    if (spool_id <= 0) {
        unassign_spool_from_slot(slot_index);
        return;
    }

    // Find the spool to verify it exists
    SpoolInfo* spool = nullptr;
    for (auto& s : mock_spools_) {
        if (s.id == spool_id) {
            spool = &s;
            break;
        }
    }

    if (!spool) {
        spdlog::warn("[MoonrakerAPIMock] assign_spool_to_slot: spool {} not found", spool_id);
        return;
    }

    slot_spool_map_[slot_index] = spool_id;
    spdlog::info("[MoonrakerAPIMock] Assigned spool {} ({} {}) to slot {}", spool_id, spool->vendor,
                 spool->filament_name, slot_index);
}

void MoonrakerSpoolmanAPIMock::unassign_spool_from_slot(int slot_index) {
    auto it = slot_spool_map_.find(slot_index);
    if (it != slot_spool_map_.end()) {
        spdlog::info("[MoonrakerAPIMock] Unassigned spool {} from slot {}", it->second, slot_index);
        slot_spool_map_.erase(it);
    }
}

int MoonrakerSpoolmanAPIMock::get_spool_for_slot(int slot_index) const {
    auto it = slot_spool_map_.find(slot_index);
    return (it != slot_spool_map_.end()) ? it->second : 0;
}

std::optional<SpoolInfo> MoonrakerSpoolmanAPIMock::get_spool_info_for_slot(int slot_index) const {
    int spool_id = get_spool_for_slot(slot_index);
    if (spool_id <= 0) {
        return std::nullopt;
    }

    for (const auto& spool : mock_spools_) {
        if (spool.id == spool_id) {
            return spool;
        }
    }
    return std::nullopt;
}

void MoonrakerSpoolmanAPIMock::consume_filament(float grams, int slot_index) {
    // Determine which spool to update
    int spool_id = mock_active_spool_id_;
    if (slot_index >= 0) {
        int slot_spool = get_spool_for_slot(slot_index);
        if (slot_spool > 0) {
            spool_id = slot_spool;
        }
    }

    if (spool_id <= 0) {
        spdlog::debug("[MoonrakerAPIMock] consume_filament: no active spool");
        return;
    }

    // Find and update the spool
    for (auto& spool : mock_spools_) {
        if (spool.id == spool_id) {
            double old_weight = spool.remaining_weight_g;
            spool.remaining_weight_g =
                std::max(0.0, spool.remaining_weight_g - static_cast<double>(grams));

            // Update remaining length proportionally
            if (spool.initial_weight_g > 0) {
                float ratio = spool.remaining_weight_g / spool.initial_weight_g;
                // Estimate ~333m per 1kg for PLA (adjust per material if needed)
                spool.remaining_length_m = ratio * 333.0f;
            }

            spdlog::debug(
                "[MoonrakerAPIMock] Consumed {:.1f}g from spool {} ({}): {:.1f}g -> {:.1f}g", grams,
                spool_id, spool.filament_name, old_weight, spool.remaining_weight_g);
            return;
        }
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
