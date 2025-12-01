// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_api_mock.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>

MoonrakerAPIMock::MoonrakerAPIMock(MoonrakerClient& client, PrinterState& state)
    : MoonrakerAPI(client, state) {
    spdlog::info("[MoonrakerAPIMock] Created - HTTP methods will use local test files");
}

void MoonrakerAPIMock::download_file(const std::string& root, const std::string& path,
                                     StringCallback on_success, ErrorCallback on_error) {
    // Build path to local test file
    // Strip any leading directory components to get just the filename
    std::string filename = path;
    size_t last_slash = path.rfind('/');
    if (last_slash != std::string::npos) {
        filename = path.substr(last_slash + 1);
    }

    std::string local_path = std::string(TEST_GCODE_DIR) + "/" + filename;

    spdlog::debug("[MoonrakerAPIMock] download_file: root='{}', path='{}' -> local: {}", root, path,
                  local_path);

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
        // File not found in test directory
        spdlog::warn("[MoonrakerAPIMock] File not found in test directory: {}", local_path);

        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::FILE_NOT_FOUND;
            err.message = "Mock file not found: " + filename;
            err.method = "download_file";
            on_error(err);
        }
    }
}

void MoonrakerAPIMock::upload_file(const std::string& root, const std::string& path,
                                   const std::string& content, SuccessCallback on_success,
                                   ErrorCallback on_error) {
    (void)on_error; // Unused - mock always succeeds

    spdlog::info("[MoonrakerAPIMock] Mock upload_file: root='{}', path='{}', size={} bytes", root,
                 path, content.size());

    // Mock always succeeds
    if (on_success) {
        on_success();
    }
}

void MoonrakerAPIMock::upload_file_with_name(const std::string& root, const std::string& path,
                                             const std::string& filename,
                                             const std::string& content,
                                             SuccessCallback on_success, ErrorCallback on_error) {
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
