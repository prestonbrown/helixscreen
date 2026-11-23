// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "usb_backend.h"

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

/**
 * @brief Mock USB backend for testing and development
 *
 * Provides simulated USB drive detection with fake drives and files.
 * Useful for:
 * - Development on macOS where real USB monitoring is complex
 * - Unit testing without real hardware
 * - Demo mode showing USB import features
 *
 * Test API allows programmatic insertion/removal of simulated drives.
 */
class UsbBackendMock : public UsbBackend {
  public:
    UsbBackendMock();
    ~UsbBackendMock() override;

    // UsbBackend Interface Implementation

    UsbError start() override;
    void stop() override;
    bool is_running() const override;

    void set_event_callback(EventCallback callback) override;

    UsbError get_connected_drives(std::vector<UsbDrive>& drives) override;
    UsbError scan_for_gcode(const std::string& mount_path, std::vector<UsbGcodeFile>& files,
                            int max_depth = 3) override;

    // Test API - For programmatic drive simulation

    /**
     * @brief Simulate inserting a USB drive
     *
     * Adds the drive to the connected drives list and fires DRIVE_INSERTED event.
     *
     * @param drive Drive to simulate insertion
     */
    void simulate_drive_insert(const UsbDrive& drive);

    /**
     * @brief Simulate removing a USB drive
     *
     * Removes the drive from connected drives list and fires DRIVE_REMOVED event.
     *
     * @param mount_path Mount path of drive to remove
     */
    void simulate_drive_remove(const std::string& mount_path);

    /**
     * @brief Add mock G-code files for a specific drive
     *
     * @param mount_path Mount path to associate files with
     * @param files Files to return when scanning this drive
     */
    void set_mock_files(const std::string& mount_path, const std::vector<UsbGcodeFile>& files);

    /**
     * @brief Clear all simulated drives and files
     */
    void clear_all();

    /// How many scan_for_gcode() calls reached this backend
    int scan_count() const {
        return scan_count_.load();
    }

    /**
     * @brief Add default demo drives with sample files
     *
     * Adds a few demo drives with realistic G-code filenames for UI testing.
     */
    void add_demo_drives();

  private:
    bool running_;
    mutable std::mutex mutex_;
    EventCallback event_callback_;
    std::vector<UsbDrive> drives_;
    std::map<std::string, std::vector<UsbGcodeFile>> mock_files_; ///< mount_path -> files

    // Demo drive scheduling (auto-adds after delay when started)
    std::thread demo_thread_;
    std::atomic<bool> demo_cancelled_{false};
    std::atomic<int> scan_count_{0};
};
