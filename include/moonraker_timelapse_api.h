// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_timelapse_api.h
 * @brief Timelapse and webcam operations via Moonraker
 *
 * Extracted from MoonrakerAPI to encapsulate all timelapse and webcam
 * functionality in a dedicated class. Uses MoonrakerClient for JSON-RPC
 * transport and HTTP for timelapse plugin settings.
 */

#pragma once

#include "i_moonraker_sub_apis.h"
#include "moonraker_error.h"
#include "moonraker_types.h"

#include <functional>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class IMoonrakerClient;
} // namespace helix

/**
 * @brief Timelapse & Webcam API operations via Moonraker
 *
 * Provides high-level operations for managing the Moonraker-Timelapse plugin
 * and querying webcam configuration. Settings methods use HTTP endpoints,
 * while render/frame/webcam methods use JSON-RPC.
 *
 * Usage:
 *   MoonrakerTimelapseAPI timelapse(client, http_base_url);
 *   timelapse.get_timelapse_settings(
 *       [](const auto& settings) { ... },
 *       [](const auto& err) { ... });
 */
class MoonrakerTimelapseAPI : public ITimelapseAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using TimelapseSettingsCallback = std::function<void(const TimelapseSettings&)>;
    using WebcamListCallback = std::function<void(const std::vector<WebcamInfo>&)>;

    /**
     * @brief Constructor
     *
     * @param client MoonrakerClient instance (must remain valid during API lifetime)
     * @param http_base_url Reference to HTTP base URL string (owned by MoonrakerAPI)
     */
    explicit MoonrakerTimelapseAPI(helix::IMoonrakerClient& client,
                                   const std::string& http_base_url);
    virtual ~MoonrakerTimelapseAPI();

    // ========================================================================
    // Timelapse Settings (HTTP-based — Moonraker-Timelapse plugin)
    // ========================================================================

    /**
     * @brief Get current timelapse settings
     *
     * Queries the Moonraker-Timelapse plugin for its current configuration.
     * Only available if has_timelapse capability is detected.
     *
     * @param on_success Callback with current settings
     * @param on_error Error callback
     */
    void get_timelapse_settings(TimelapseSettingsCallback on_success,
                                ErrorCallback on_error) override;

    /**
     * @brief Update timelapse settings
     *
     * Configures the Moonraker-Timelapse plugin with new settings.
     * Changes take effect for the next print.
     *
     * @param settings New settings to apply
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void set_timelapse_settings(const TimelapseSettings& settings, SuccessCallback on_success,
                                ErrorCallback on_error) override;

    /**
     * @brief Enable or disable timelapse for current/next print
     *
     * Convenience method to toggle just the enabled state without
     * changing other settings.
     *
     * @param enabled true to enable, false to disable
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void set_timelapse_enabled(bool enabled, SuccessCallback on_success,
                               ErrorCallback on_error) override;

    // ========================================================================
    // Timelapse Render / Frame Operations (JSON-RPC)
    // ========================================================================

    /**
     * @brief Trigger timelapse video rendering
     *
     * Starts the rendering process for captured frames into a video file.
     * Progress is reported via notify_timelapse_event WebSocket events.
     */
    void render_timelapse(SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Save timelapse frames without rendering
     *
     * Saves captured frame files for later processing.
     */
    void save_timelapse_frames(SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Get information about the last captured frame
     */
    void get_last_frame_info(std::function<void(const LastFrameInfo&)> on_success,
                             ErrorCallback on_error) override;

    // ========================================================================
    // Webcam Operations (JSON-RPC)
    // ========================================================================

    /**
     * @brief Get list of configured webcams
     *
     * Queries Moonraker for configured webcams. Used to detect if the printer
     * has a camera, which is a prerequisite for timelapse setup.
     *
     * @param on_success Callback with vector of webcam info
     * @param on_error Error callback
     */
    void get_webcam_list(WebcamListCallback on_success, ErrorCallback on_error) override;

  protected:
    helix::IMoonrakerClient& client_;
    const std::string& http_base_url_;
};
