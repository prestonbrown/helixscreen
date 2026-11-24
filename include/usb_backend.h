// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief USB operation result codes
 */
enum class UsbResult {
    SUCCESS = 0,       ///< Operation succeeded
    PERMISSION_DENIED, ///< Insufficient permissions to monitor devices
    NOT_SUPPORTED,     ///< Platform doesn't support USB monitoring
    BACKEND_ERROR,     ///< Internal backend error
    NOT_INITIALIZED,   ///< Backend not started/initialized
    DRIVE_NOT_FOUND,   ///< Specified drive not mounted
    SCAN_FAILED,       ///< Failed to scan directory
    UNKNOWN_ERROR      ///< Unexpected error condition
};

/**
 * @brief Detailed error information for USB operations
 */
struct UsbError {
    UsbResult result;          ///< Primary error code
    std::string technical_msg; ///< Technical details for logging/debugging
    std::string user_msg;      ///< User-friendly message for UI display

    UsbError(UsbResult r = UsbResult::SUCCESS, const std::string& tech = "",
             const std::string& user = "")
        : result(r), technical_msg(tech), user_msg(user) {}

    bool success() const {
        return result == UsbResult::SUCCESS;
    }
    operator bool() const {
        return success();
    }
};

/**
 * @brief USB drive information
 *
 * Carries no capacity fields: free space on a removable FAT volume costs a
 * full FAT scan on the first query after mount (seconds on embedded flash),
 * so whoever displays capacity computes it on demand - never the detection
 * path that decides a drive exists.
 */
struct UsbDrive {
    std::string mount_path; ///< Mount point path ("/media/usb0" or "/Volumes/USBDRIVE")
    std::string device;     ///< Device path ("/dev/sda1")
    std::string label;      ///< Volume label ("USBDRIVE")

    UsbDrive() = default;

    UsbDrive(const std::string& mount, const std::string& dev, const std::string& lbl)
        : mount_path(mount), device(dev), label(lbl) {}
};

/**
 * @brief G-code file information found on USB drive
 */
struct UsbGcodeFile {
    std::string path;      ///< Full path to file on USB drive
    std::string filename;  ///< Just the filename (basename)
    uint64_t size_bytes;   ///< File size in bytes
    int64_t modified_time; ///< Last modified timestamp (Unix epoch)

    UsbGcodeFile() : size_bytes(0), modified_time(0) {}

    UsbGcodeFile(const std::string& p, const std::string& name, uint64_t size, int64_t mtime)
        : path(p), filename(name), size_bytes(size), modified_time(mtime) {}
};

/**
 * @brief USB event types for callbacks
 */
enum class UsbEvent {
    DRIVE_INSERTED, ///< USB drive was mounted
    DRIVE_REMOVED   ///< USB drive was unmounted
};

/**
 * @brief Abstract USB backend interface
 *
 * Provides a clean, platform-agnostic API for USB drive monitoring.
 * Concrete implementations handle platform-specific details:
 * - UsbBackendLinux: polls /proc/self/mountinfo for mount table changes,
 *   parses /proc/mounts
 * - UsbBackendMock: Simulator mode with fake drives
 *
 * No other platform has a backend: create() returns nullptr there.
 *
 * Design principles:
 * - Hide all platform-specific details from UsbManager
 * - Provide event-based notification for drive changes
 * - Thread-safe operations where needed
 * - Clean error handling with meaningful messages
 */
class UsbBackend {
  public:
    virtual ~UsbBackend() = default;

    /**
     * @brief Callback type for USB drive events
     * @param event The type of event (inserted/removed)
     * @param drive Information about the affected drive
     */
    using EventCallback = std::function<void(UsbEvent event, const UsbDrive& drive)>;

    // Lifecycle Management

    /**
     * @brief Initialize and start the USB backend
     *
     * Begins monitoring for USB drive mount/unmount events.
     * May start background threads for event processing.
     *
     * @return UsbError with detailed status information
     */
    virtual UsbError start() = 0;

    /**
     * @brief Stop the USB backend
     *
     * Cleanly shuts down background threads and stops monitoring.
     */
    virtual void stop() = 0;

    /**
     * @brief Check if backend is currently running/initialized
     *
     * @return true if backend is active and monitoring
     */
    virtual bool is_running() const = 0;

    // Event System

    /**
     * @brief Register callback for USB drive events
     *
     * Events are delivered asynchronously and may arrive from background threads.
     * Ensure thread safety in callback implementations.
     *
     * @param callback Handler function for drive events
     */
    virtual void set_event_callback(EventCallback callback) = 0;

    // Drive Queries

    /**
     * @brief Get list of currently mounted USB drives
     *
     * @param[out] drives Vector to populate with mounted drives
     * @return UsbError with detailed status information
     */
    virtual UsbError get_connected_drives(std::vector<UsbDrive>& drives) = 0;

    /**
     * @brief Scan a USB drive for G-code files
     *
     * Recursively scans the mount point for files with .gcode extension.
     *
     * @param mount_path Path to scan (should be a mounted USB drive)
     * @param[out] files Vector to populate with found G-code files
     * @param max_depth Maximum directory depth to scan (0 = root only, -1 = unlimited)
     * @return UsbError with detailed status information
     */
    virtual UsbError scan_for_gcode(const std::string& mount_path, std::vector<UsbGcodeFile>& files,
                                    int max_depth = 3) = 0;

    // Factory Method

    /**
     * @brief Create appropriate backend for current platform
     *
     * - Linux: UsbBackendLinux (mount table poll)
     * - Test mode: UsbBackendMock (simulator with fake data)
     * - Anything else: nullptr
     *
     * The backend comes back unstarted, so the caller can attach its event
     * callback before start().
     *
     * @param force_mock If true, always return mock backend (for testing)
     * @return Unique pointer to backend instance, or nullptr
     */
    static std::unique_ptr<UsbBackend> create(bool force_mock = false);
};
