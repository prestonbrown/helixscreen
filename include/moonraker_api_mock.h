// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MOONRAKER_API_MOCK_H
#define MOONRAKER_API_MOCK_H

#include "moonraker_api.h"

#include <string>

/**
 * @brief Mock MoonrakerAPI for testing without real printer connection
 *
 * Overrides HTTP file transfer methods to use local test files instead
 * of making actual HTTP requests to a Moonraker server.
 *
 * Usage:
 *   MoonrakerClientMock mock_client;
 *   PrinterState state;
 *   MoonrakerAPIMock mock_api(mock_client, state);
 *   // mock_api.download_file() now reads from assets/test_gcodes/
 */
class MoonrakerAPIMock : public MoonrakerAPI {
  public:
    /**
     * @brief Construct mock API
     *
     * @param client MoonrakerClient instance (typically MoonrakerClientMock)
     * @param state PrinterState instance
     */
    MoonrakerAPIMock(MoonrakerClient& client, PrinterState& state);

    ~MoonrakerAPIMock() override = default;

    // ========================================================================
    // Overridden HTTP File Transfer Methods (use local files instead of HTTP)
    // ========================================================================

    /**
     * @brief Download file from local test directory
     *
     * Instead of making HTTP request, reads from assets/test_gcodes/{path}.
     * For files not in test directory, returns mock content.
     *
     * @param root Root directory (ignored in mock - always uses test_gcodes)
     * @param path File path
     * @param on_success Callback with file content
     * @param on_error Error callback
     */
    void download_file(const std::string& root, const std::string& path,
                       StringCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Mock file upload (logs but doesn't write)
     *
     * Logs the upload request but doesn't actually write files.
     * Always calls success callback.
     *
     * @param root Root directory
     * @param path Destination path
     * @param content File content
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void upload_file(const std::string& root, const std::string& path,
                     const std::string& content, SuccessCallback on_success,
                     ErrorCallback on_error) override;

    /**
     * @brief Mock file upload with custom filename (logs but doesn't write)
     *
     * Logs the upload request but doesn't actually write files.
     * Always calls success callback.
     */
    void upload_file_with_name(const std::string& root, const std::string& path,
                               const std::string& filename, const std::string& content,
                               SuccessCallback on_success, ErrorCallback on_error) override;

  private:
    static constexpr const char* TEST_GCODE_DIR = "assets/test_gcodes";
};

#endif // MOONRAKER_API_MOCK_H
