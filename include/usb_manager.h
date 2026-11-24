// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "usb_backend.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief High-level USB drive manager
 *
 * Coordinates USB backend lifecycle and provides application-level API for:
 * - Starting/stopping USB monitoring
 * - Receiving drive insert/remove notifications
 * - Querying available drives and G-code files
 *
 * This is the class that application code should interact with, rather than
 * using the backend directly.
 *
 * Usage:
 * @code
 * UsbManager usb;
 * usb.set_drive_callback([](UsbEvent event, const UsbDrive& drive) {
 *     if (event == UsbEvent::DRIVE_INSERTED) {
 *         show_toast("USB drive detected: " + drive.label);
 *     }
 * });
 * usb.start();
 * @endcode
 */
class UsbManager {
  public:
    /**
     * @brief Callback type for USB drive events
     */
    using DriveCallback = std::function<void(UsbEvent event, const UsbDrive& drive)>;

    /**
     * @brief Construct USB manager
     * @param force_mock If true, always use mock backend (for testing)
     */
    explicit UsbManager(bool force_mock = false);

    ~UsbManager();

    // Non-copyable, non-movable
    UsbManager(const UsbManager&) = delete;
    UsbManager& operator=(const UsbManager&) = delete;
    UsbManager(UsbManager&&) = delete;
    UsbManager& operator=(UsbManager&&) = delete;

    // Lifecycle

    /**
     * @brief Start USB monitoring
     *
     * Initializes the backend and begins monitoring for USB drive events.
     *
     * @return true if successfully started
     */
    bool start();

    /**
     * @brief Stop USB monitoring
     *
     * Stops the backend and cleans up resources.
     */
    void stop();

    /**
     * @brief Check if USB monitoring is active
     */
    bool is_running() const;

    // Event Callbacks

    /**
     * @brief Set callback for drive events
     *
     * The callback is invoked when USB drives are inserted or removed.
     * May be called from a background thread - ensure thread safety.
     *
     * @param callback Function to call on drive events
     */
    void set_drive_callback(DriveCallback callback);

    // Drive Queries

    /**
     * @brief Get list of currently connected USB drives
     *
     * @return Vector of connected drives (empty if none or not running)
     */
    std::vector<UsbDrive> get_drives() const;

    /**
     * @brief Scan a drive for G-code files
     *
     * @param mount_path Mount point of the drive to scan
     * @param max_depth Maximum directory depth (-1 for unlimited)
     * @return Vector of found G-code files (empty on error)
     */
    std::vector<UsbGcodeFile> scan_for_gcode(const std::string& mount_path,
                                             int max_depth = 3) const;

    /**
     * @brief The backend, for work that must not reach back into this manager
     *
     * A worker thread that may outlive the manager scans through this rather
     * than through scan_for_gcode(). The manager's destructor stops the
     * backend, after which its scans return nothing.
     */
    std::shared_ptr<UsbBackend> backend_snapshot() const;

    // Test API (for UsbBackendMock)

    /**
     * @brief Get the underlying backend (for testing)
     *
     * @return Pointer to backend, or nullptr if not initialized
     */
    UsbBackend* get_backend();

  private:
    void on_backend_event(UsbEvent event, const UsbDrive& drive);

    // Shared so a scan can walk the stick without holding mutex_: stop()
    // dropping its reference mid-walk leaves the scan's copy alive.
    std::shared_ptr<UsbBackend> backend_;
    DriveCallback drive_callback_;
    mutable std::mutex mutex_;
    bool force_mock_;
};
