// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(__linux__) && !defined(__ANDROID__)

#include "usb_backend_linux.h"

#include "ui_filename_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

UsbBackendLinux::UsbBackendLinux() {
    spdlog::debug("[UsbBackendLinux] Created");
}

UsbBackendLinux::~UsbBackendLinux() {
    stop();
}

UsbError UsbBackendLinux::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_) {
        return UsbError(UsbResult::SUCCESS);
    }

    // Event path: /proc/self/mountinfo is pollable (POLLPRI on change) on every
    // Linux we ship. procfs never generates inotify events, so inotify cannot
    // watch the mount table at all.
    mountinfo_fd_ = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (mountinfo_fd_ < 0) {
        spdlog::warn("[UsbBackendLinux] Cannot open /proc/self/mountinfo ({}), "
                     "using 1s content polling",
                     strerror(errno));
        switch_to_content_polling();
    } else {
        use_content_polling_ = false;
    }

    // Get initial drive list
    cached_drives_ = parse_mounts();
    spdlog::info("[UsbBackendLinux] Initial scan found {} USB drives (content-polling={})",
                 cached_drives_.size(), use_content_polling_.load());

    // Start monitor thread. Wrap — EAGAIN throws ([L083]).
    stop_requested_ = false;
    running_ = true;
    try {
        monitor_thread_ = std::thread(&UsbBackendLinux::monitor_thread_func, this);
    } catch (const std::system_error& e) {
        spdlog::error("[UsbBackendLinux] Failed to spawn monitor thread: {}", e.what());
        running_ = false;
        if (mountinfo_fd_ >= 0) {
            close(mountinfo_fd_);
            mountinfo_fd_ = -1;
        }
        return UsbError(UsbResult::BACKEND_ERROR, "system busy");
    }

    spdlog::info("[UsbBackendLinux] Started (mode={})",
                 use_content_polling_.load() ? "content-poll" : "mountinfo-poll");
    return UsbError(UsbResult::SUCCESS);
}

void UsbBackendLinux::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        stop_requested_ = true;
    }

    // Wait for monitor thread
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }

    // Cleanup the mountinfo fd. The monitor thread is joined above, so only
    // this path and a failed start() touch the fd outside the thread.
    if (mountinfo_fd_ >= 0) {
        close(mountinfo_fd_);
        mountinfo_fd_ = -1;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        cached_drives_.clear();
        use_content_polling_ = false;
        last_mounts_content_.clear();
    }

    spdlog::info("[UsbBackendLinux] Stopped");
}

bool UsbBackendLinux::is_running() const {
    return running_;
}

void UsbBackendLinux::set_event_callback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_callback_ = std::move(callback);
}

UsbError UsbBackendLinux::get_connected_drives(std::vector<UsbDrive>& drives) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!running_) {
        return UsbError(UsbResult::NOT_INITIALIZED, "Backend not started",
                        "USB monitoring not active");
    }

    drives = cached_drives_;
    return UsbError(UsbResult::SUCCESS);
}

UsbError UsbBackendLinux::scan_for_gcode(const std::string& mount_path,
                                         std::vector<UsbGcodeFile>& files, int max_depth) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!running_) {
        return UsbError(UsbResult::NOT_INITIALIZED, "Backend not started",
                        "USB monitoring not active");
    }

    // Verify drive exists
    auto it = std::find_if(cached_drives_.begin(), cached_drives_.end(),
                           [&mount_path](const UsbDrive& d) { return d.mount_path == mount_path; });
    if (it == cached_drives_.end()) {
        return UsbError(UsbResult::DRIVE_NOT_FOUND, "Drive not mounted: " + mount_path,
                        "USB drive not connected");
    }

    files.clear();
    scan_directory(mount_path, files, 0, max_depth);

    spdlog::debug("[UsbBackendLinux] Found {} G-code files on {}", files.size(), mount_path);
    return UsbError(UsbResult::SUCCESS);
}

std::vector<UsbDrive> UsbBackendLinux::parse_mounts() {
    std::vector<UsbDrive> drives;

    std::ifstream mounts("/proc/mounts");
    if (!mounts.is_open()) {
        spdlog::warn("[UsbBackendLinux] Failed to open /proc/mounts");
        return drives;
    }

    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream iss(line);
        std::string device, mount_point, fs_type, options;

        if (!(iss >> device >> mount_point >> fs_type >> options)) {
            continue;
        }

        // Unescape mount point (spaces are encoded as \040)
        size_t pos;
        while ((pos = mount_point.find("\\040")) != std::string::npos) {
            mount_point.replace(pos, 4, " ");
        }

        if (is_usb_mount(device, mount_point, fs_type)) {
            UsbDrive drive;
            drive.device = device;
            drive.mount_path = mount_point;
            drive.label = get_volume_label(device, mount_point);
            get_capacity(mount_point, drive.total_bytes, drive.available_bytes);

            spdlog::debug("[UsbBackendLinux] Found USB drive: {} at {} ({})", drive.label,
                          drive.mount_path, drive.device);
            drives.push_back(drive);
        }
    }

    return drives;
}

bool UsbBackendLinux::is_usb_mount(const std::string& device, const std::string& mount_point,
                                   const std::string& fs_type) {
    // Must be a block device (starts with /dev/)
    if (device.find("/dev/") != 0) {
        return false;
    }

    // Common USB mount points (includes /tmp/udisk/ for Creality K1C, #610)
    bool is_usb_path =
        (mount_point.find("/media/") == 0 || mount_point.find("/mnt/") == 0 ||
         mount_point.find("/run/media/") == 0 || mount_point.find("/tmp/udisk/") == 0);
    if (!is_usb_path) {
        return false;
    }

    // Common USB filesystems. msdos is what the kernel registers a FAT mount
    // without long-filename support as, which is what auto-mounting a FAT
    // stick on printer firmware produces. iso9660 and f2fs stay out: a
    // loop-mounted ISO image under /media would pass the /media fallback below
    // and surface a disk image as a drive, and f2fs is the boards' own
    // internal-flash filesystem with no removable use observed.
    bool is_usb_fs =
        (fs_type == "vfat" || fs_type == "msdos" || fs_type == "exfat" || fs_type == "ntfs" ||
         fs_type == "ntfs3" || fs_type == "ext4" || fs_type == "ext3" || fs_type == "fuseblk");
    if (!is_usb_fs) {
        return false;
    }

    // Check if device looks like a removable drive
    // USB drives typically show up as /dev/sd[a-z][0-9] or /dev/nvme*
    // We can also check /sys/block/*/removable
    std::string base_device = device;

    // Extract base device name (e.g., /dev/sda1 -> sda)
    size_t dev_start = device.rfind('/');
    if (dev_start != std::string::npos) {
        std::string dev_name = device.substr(dev_start + 1);
        // Remove partition number
        while (!dev_name.empty() && std::isdigit(dev_name.back())) {
            dev_name.pop_back();
        }

        // Check if removable
        std::string removable_path = "/sys/block/" + dev_name + "/removable";
        std::ifstream removable_file(removable_path);
        if (removable_file.is_open()) {
            int removable = 0;
            removable_file >> removable;
            if (removable == 1) {
                return true;
            }
        }

        // Also check for USB in device path (some USB drives aren't marked removable)
        std::string uevent_path = "/sys/block/" + dev_name + "/device/uevent";
        std::ifstream uevent_file(uevent_path);
        if (uevent_file.is_open()) {
            std::string line;
            while (std::getline(uevent_file, line)) {
                if (line.find("DRIVER=usb-storage") != std::string::npos ||
                    line.find("DRIVER=uas") != std::string::npos) {
                    return true;
                }
            }
        }
    }

    // Fallback: if on /media/ and has USB-like filesystem, assume it's USB
    // This catches cases where sysfs checks fail
    return (mount_point.find("/media/") == 0 && is_usb_fs);
}

std::string UsbBackendLinux::get_volume_label(const std::string& device,
                                              const std::string& mount_point) {
    // Try to get label from /dev/disk/by-label/
    // This is a bit tricky - we need to reverse-lookup

    // First try: extract from mount point (often the label is used)
    size_t last_slash = mount_point.rfind('/');
    if (last_slash != std::string::npos && last_slash + 1 < mount_point.size()) {
        std::string possible_label = mount_point.substr(last_slash + 1);
        // If it's not just the device name, use it
        if (possible_label.find("sd") != 0 && possible_label.find("nvme") != 0) {
            return possible_label;
        }
    }

    // Try blkid-style lookup via /dev/disk/by-label
    DIR* dir = opendir("/dev/disk/by-label");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') {
                continue;
            }

            std::string link_path = std::string("/dev/disk/by-label/") + entry->d_name;
            char resolved[PATH_MAX];
            if (realpath(link_path.c_str(), resolved) != nullptr) {
                if (device == resolved) {
                    // entry points into dir's buffer, so copy the name before closedir
                    std::string label = entry->d_name;
                    closedir(dir);
                    // Unescape label (spaces encoded as \x20)
                    size_t pos;
                    while ((pos = label.find("\\x20")) != std::string::npos) {
                        label.replace(pos, 4, " ");
                    }
                    return label;
                }
            }
        }
        closedir(dir);
    }

    // Fallback: use device name
    size_t dev_start = device.rfind('/');
    if (dev_start != std::string::npos) {
        return device.substr(dev_start + 1);
    }

    return "USB Drive";
}

void UsbBackendLinux::get_capacity(const std::string& mount_point, uint64_t& total,
                                   uint64_t& available) {
    struct statvfs stat;
    if (statvfs(mount_point.c_str(), &stat) == 0) {
        total = static_cast<uint64_t>(stat.f_blocks) * stat.f_frsize;
        available = static_cast<uint64_t>(stat.f_bavail) * stat.f_frsize;
    } else {
        total = 0;
        available = 0;
    }
}

void UsbBackendLinux::drain_mountinfo_fd() {
    // Procfs has no event queue: reading to EOF and rewinding is what re-arms
    // POLLPRI for the next mount change.
    char buf[4096];
    (void)lseek(mountinfo_fd_, 0, SEEK_SET);
    while (read(mountinfo_fd_, buf, sizeof(buf)) > 0) {
    }
    (void)lseek(mountinfo_fd_, 0, SEEK_SET);
}

void UsbBackendLinux::switch_to_content_polling() {
    if (mountinfo_fd_ >= 0) {
        close(mountinfo_fd_);
        mountinfo_fd_ = -1;
    }
    use_content_polling_ = true;
    last_mounts_content_ = read_mounts_content();
}

void UsbBackendLinux::monitor_thread_func() {
    spdlog::debug("[UsbBackendLinux] Monitor thread started (mode={})",
                  use_content_polling_.load() ? "content-poll" : "mountinfo-poll");

    // Safety re-parse cadence: bounds how stale cached_drives_ can get if the
    // event mechanism misbehaves on some kernel. Reading /proc/mounts is cheap
    // enough for the slowest board we ship. The diff below is a no-op when
    // nothing changed, so this costs nothing in the steady state.
    constexpr auto kSafetyReparseInterval = std::chrono::seconds(10);
    auto last_safety_parse = std::chrono::steady_clock::now();

    while (!stop_requested_) {
        bool mounts_changed = false;

        if (use_content_polling_.load()) {
            // Fallback: compare /proc/mounts content periodically
            // Note: We compare content rather than mtime because /proc/mounts is often
            // a symlink to /proc/self/mounts, and symlink mtime never changes.
            // Sleep first to avoid tight loop
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            if (stop_requested_) {
                break;
            }

            std::string current_content = read_mounts_content();
            if (current_content != last_mounts_content_) {
                spdlog::debug("[UsbBackendLinux] /proc/mounts content changed");
                last_mounts_content_ = current_content;
                mounts_changed = true;
            }
        } else {
            // Event path: procfs mount files signal a change via POLLPRI
            // (reported together with POLLERR). The 500ms timeout keeps
            // stop_requested_ honoured promptly.
            struct pollfd pfd;
            pfd.fd = mountinfo_fd_;
            pfd.events = POLLPRI;

            int ret = poll(&pfd, 1, 500);
            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                }
                spdlog::error("[UsbBackendLinux] poll(mountinfo) failed: {}, "
                              "switching to 1s content polling",
                              strerror(errno));
                switch_to_content_polling();
                continue;
            }

            if (ret > 0 && (pfd.revents & (POLLPRI | POLLERR))) {
                drain_mountinfo_fd();
                mounts_changed = true;
            }
        }

        // Safety re-parse: convergence is guaranteed by the clock, not by the
        // event mechanism.
        auto now = std::chrono::steady_clock::now();
        if (now - last_safety_parse >= kSafetyReparseInterval) {
            last_safety_parse = now;
            mounts_changed = true;
        }

        if (mounts_changed) {
            spdlog::debug("[UsbBackendLinux] Mount change detected");

            // Re-parse mounts
            auto new_drives = parse_mounts();

            // Compare with cached drives to find additions/removals
            EventCallback callback_copy;
            std::vector<UsbDrive> added, removed;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                callback_copy = event_callback_;

                // Find removed drives
                for (const auto& old_drive : cached_drives_) {
                    auto it = std::find_if(new_drives.begin(), new_drives.end(),
                                           [&old_drive](const UsbDrive& d) {
                                               return d.mount_path == old_drive.mount_path;
                                           });
                    if (it == new_drives.end()) {
                        removed.push_back(old_drive);
                    }
                }

                // Find added drives
                for (const auto& new_drive : new_drives) {
                    auto it = std::find_if(cached_drives_.begin(), cached_drives_.end(),
                                           [&new_drive](const UsbDrive& d) {
                                               return d.mount_path == new_drive.mount_path;
                                           });
                    if (it == cached_drives_.end()) {
                        added.push_back(new_drive);
                    }
                }

                cached_drives_ = new_drives;
            }

            // Fire callbacks outside lock
            if (callback_copy) {
                for (const auto& drive : removed) {
                    spdlog::info("[UsbBackendLinux] Drive removed: {} ({})", drive.label,
                                 drive.mount_path);
                    callback_copy(UsbEvent::DRIVE_REMOVED, drive);
                }
                for (const auto& drive : added) {
                    spdlog::info("[UsbBackendLinux] Drive inserted: {} ({})", drive.label,
                                 drive.mount_path);
                    callback_copy(UsbEvent::DRIVE_INSERTED, drive);
                }
            }
        }
    }

    spdlog::debug("[UsbBackendLinux] Monitor thread stopped");
}

void UsbBackendLinux::scan_directory(const std::string& path, std::vector<UsbGcodeFile>& files,
                                     int current_depth, int max_depth) {
    if (max_depth >= 0 && current_depth > max_depth) {
        return;
    }

    DIR* dir = opendir(path.c_str());
    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Skip . and ..
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        std::string full_path = path + "/" + entry->d_name;

        struct stat st;
        if (stat(full_path.c_str(), &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            // Recurse into subdirectory
            scan_directory(full_path, files, current_depth + 1, max_depth);
        } else if (S_ISREG(st.st_mode)) {
            const std::string name = entry->d_name;
            // Shared printable-extension predicate, same rule the Moonraker
            // file list applies. A FAT mount without long filenames yields
            // 8.3 upper-case names like 3DBENC~1.GCO.
            if (helix::gcode::has_printable_extension(name)) {
                UsbGcodeFile file;
                file.path = full_path;
                file.filename = name;
                file.size_bytes = static_cast<uint64_t>(st.st_size);
                file.modified_time = static_cast<int64_t>(st.st_mtime);
                files.push_back(file);
            }
        }
    }

    closedir(dir);
}

std::string UsbBackendLinux::read_mounts_content() {
    std::ifstream mounts("/proc/mounts");
    if (!mounts.is_open()) {
        return "";
    }

    std::stringstream buffer;
    buffer << mounts.rdbuf();
    return buffer.str();
}

#endif // __linux__ && !__ANDROID__
