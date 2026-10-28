// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __linux__

#include "usb_backend.h"

#include <atomic>
#include <mutex>
#include <thread>

/**
 * @brief Linux USB backend watching /proc/self/mountinfo
 *
 * Monitors USB drive mount/unmount events using:
 * - poll(POLLPRI|POLLERR) on /proc/self/mountinfo for event-driven mount
 *   change notification (procfs has no inotify support, but its mount files
 *   are pollable: POLLPRI fires on a mount table change once the file has been
 *   read to EOF and rewound)
 * - Parsing /proc/mounts to detect USB drives (looking for /dev/sd* on /media
 *   or /mnt)
 * - statvfs() for capacity information
 *
 * Design notes:
 * - /proc/mounts changes whenever any filesystem is mounted/unmounted
 * - We filter for USB-like mounts (block devices on common USB mount points)
 * - A 1s /proc/mounts content compare takes over when mountinfo cannot be
 *   opened or its poll fails
 * - Independently of the mechanism, a safety re-parse runs every 10s so
 *   detection converges even on a kernel where the event path misbehaves;
 *   a monitor that can silently never fire again is the defect this design
 *   excludes
 */
class UsbBackendLinux : public UsbBackend {
  public:
    UsbBackendLinux();
    ~UsbBackendLinux() override;

    // ========================================================================
    // UsbBackend Interface Implementation
    // ========================================================================

    UsbError start() override;
    void stop() override;
    bool is_running() const override;

    void set_event_callback(EventCallback callback) override;

    UsbError get_connected_drives(std::vector<UsbDrive>& drives) override;
    UsbError scan_for_gcode(const std::string& mount_path, std::vector<UsbGcodeFile>& files,
                            int max_depth = 3) override;

    // ========================================================================
    // Testable surface (pure classification / filesystem walk, no thread state)
    // ========================================================================

    /**
     * @brief Check if a mount entry looks like a USB drive
     * @param device Device path (e.g., /dev/sda1)
     * @param mount_point Mount point path
     * @param fs_type Filesystem type
     * @return true if this appears to be a USB drive
     */
    bool is_usb_mount(const std::string& device, const std::string& mount_point,
                      const std::string& fs_type);

    /**
     * @brief Recursively scan a directory for printable G-code files
     */
    void scan_directory(const std::string& path, std::vector<UsbGcodeFile>& files,
                        int current_depth, int max_depth);

    /**
     * @brief Is the mountinfo event path active? (valid while running)
     *
     * False means the 1s content-compare fallback is in effect.
     */
    bool using_mountinfo_events() const {
        return !use_content_polling_.load();
    }

  private:
    /**
     * @brief Parse /proc/mounts and return USB drives
     */
    std::vector<UsbDrive> parse_mounts();

    /**
     * @brief Get volume label for a device
     */
    std::string get_volume_label(const std::string& device, const std::string& mount_point);

    /**
     * @brief Get capacity info for a mount point
     */
    void get_capacity(const std::string& mount_point, uint64_t& total, uint64_t& available);

    /**
     * @brief Background thread function - monitors the mount table
     */
    void monitor_thread_func();

    /**
     * @brief Read /proc/self/mountinfo to EOF and rewind
     *
     * Procfs has no event queue: draining and rewinding the file is what
     * re-arms POLLPRI for the next mount change.
     */
    void drain_mountinfo_fd();

    /**
     * @brief Abandon the event path and fall back to 1s content compare
     *
     * Closes the mountinfo fd and seeds the content snapshot. Callable from
     * start() (mountinfo unopenable) and the monitor thread (poll failed).
     */
    void switch_to_content_polling();

    // State
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    mutable std::mutex mutex_;
    EventCallback event_callback_;
    std::vector<UsbDrive> cached_drives_;

    // Event path: pollable fd on /proc/self/mountinfo
    int mountinfo_fd_{-1};
    std::thread monitor_thread_;

    // Content-compare fallback. Atomic: the monitor thread can demote itself
    // to this mode after a poll failure while a reader checks the mode.
    // We compare actual content rather than mtime because /proc/mounts
    // is often a symlink to /proc/self/mounts, and symlink mtime never changes.
    std::atomic<bool> use_content_polling_{false};
    std::string last_mounts_content_;

    /**
     * @brief Read contents of /proc/mounts for polling comparison
     */
    std::string read_mounts_content();
};

#endif // __linux__
