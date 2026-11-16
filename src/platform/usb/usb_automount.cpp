// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(__linux__) && !defined(__ANDROID__)

#include "usb_automount.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

namespace helix::usb {

std::vector<MountAttempt> automount_ladder() {
    return {
        {"vfat", "ro,noatime"},
        {"vfat", "ro,noatime,iocharset=utf8"},
        {"vfat", "ro,noatime,iocharset=iso8859-1"},
        {"vfat", "ro,noatime,codepage=437,iocharset=iso8859-1"},
        {"exfat", "ro,noatime"},
        {"ntfs3", "ro,noatime"},
        {"msdos", "ro,noatime"},
        {"msdos", "ro,noatime,iocharset=utf8"},
        {"msdos", "ro,noatime,iocharset=iso8859-1"},
        {"msdos", "ro,noatime,codepage=437,iocharset=iso8859-1"},
    };
}

std::string automount_mount_point(const std::string& device_node) {
    // /mnt/usb sits inside every prefix is_usb_mount accepts and matches the
    // boards' own convention. The leaf is the device basename (sda1), which
    // never contains a path separator.
    size_t slash = device_node.rfind('/');
    const std::string name =
        slash == std::string::npos ? device_node : device_node.substr(slash + 1);
    return "/mnt/usb/" + name;
}

namespace {

/// Does this /sys/block/<disk> node belong to a removable USB disk?
bool is_usb_disk_sysfs(const std::string& sysfs_dir) {
    // removable=1 covers the classic USB mass-storage stick; the driver check
    // covers devices that do not set the flag. Same pair of signals
    // UsbBackendLinux::is_usb_mount accepts.
    {
        std::ifstream f(sysfs_dir + "/removable");
        int removable = 0;
        if (f.is_open() && (f >> removable) && removable == 1) {
            return true;
        }
    }
    std::ifstream uevent(sysfs_dir + "/device/uevent");
    std::string line;
    while (std::getline(uevent, line)) {
        if (line == "DRIVER=usb-storage" || line == "DRIVER=uas") {
            return true;
        }
    }
    return false;
}

/// Partition names inside /sys/block/<disk> (sda1, sda2, ...), sorted.
std::vector<std::string> disk_partitions(const std::string& disk) {
    std::vector<std::string> parts;
    const std::string dir_path = "/sys/block/" + disk;
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) {
        return parts;
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        const std::string name(entry->d_name);
        if (name.size() > disk.size() && name.compare(0, disk.size(), disk) == 0 &&
            std::isdigit(static_cast<unsigned char>(name[disk.size()]))) {
            parts.push_back(name);
        }
    }
    closedir(dir);
    std::sort(parts.begin(), parts.end());
    return parts;
}

bool is_directory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/// /proc/sys/kernel/hotplug prints an unregistered helper as an empty line.
bool hotplug_helper_registered(const std::string& line) {
    std::string trimmed = line;
    trimmed.erase(std::remove_if(trimmed.begin(), trimmed.end(),
                                 [](unsigned char c) { return std::isspace(c) != 0; }),
                  trimmed.end());
    return !trimmed.empty() && trimmed != "(none)";
}

/// Ask the system whether anything else mounts USB sticks. Two filesystem
/// traces cover the primary mounters a Linux board ships with: udev's control
/// socket (created by the daemon itself at startup; udisks2 and the desktop
/// automounters all sit on top of udev) and the kernel hotplug helper (how
/// mdev and vendor hotplug scripts are registered). ABSENT requires both
/// signals positively observed; anything unreadable reads UNKNOWN and keeps
/// the grace, since fighting another mounter is what the grace prevents.
MounterPresence probe_primary_mounter() {
    struct stat st {};
    const bool udev_running = (::stat("/run/udev/control", &st) == 0);

    std::string helper_line;
    bool hotplug_readable = false;
    if (std::ifstream f("/proc/sys/kernel/hotplug"); f.is_open()) {
        std::getline(f, helper_line);
        hotplug_readable = true;
    }
    const bool helper = hotplug_helper_registered(helper_line);

    if (udev_running || helper) {
        spdlog::debug("[UsbAutomount] Primary mounter present (udev daemon: {}, hotplug helper: "
                      "{}) - keeping mount grace",
                      udev_running, helper);
        return MounterPresence::PRESENT;
    }
    if (hotplug_readable) {
        spdlog::debug("[UsbAutomount] No primary mounter (no udev control socket, no hotplug "
                      "helper) - skipping mount grace");
        return MounterPresence::ABSENT;
    }
    spdlog::debug("[UsbAutomount] Primary mounter status unobservable - keeping mount grace");
    return MounterPresence::UNKNOWN;
}

class SystemMountOps final : public MountOps {
  public:
    bool is_root() override {
        return ::geteuid() == 0;
    }

    std::vector<std::string> mounted_devices() override {
        std::vector<std::string> devices;
        std::ifstream mounts("/proc/mounts");
        std::string line;
        while (std::getline(mounts, line)) {
            std::istringstream iss(line);
            std::string device;
            if (iss >> device) {
                devices.push_back(device);
            }
        }
        return devices;
    }

    std::vector<std::string> removable_block_devices() override {
        std::vector<std::string> nodes;
        DIR* dir = opendir("/sys/block");
        if (!dir) {
            return nodes;
        }
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            const std::string disk(entry->d_name);
            if (disk.rfind("sd", 0) != 0) {
                continue;
            }
            if (!is_usb_disk_sysfs("/sys/block/" + disk)) {
                continue;
            }
            const auto partitions = disk_partitions(disk);
            if (!partitions.empty()) {
                for (const auto& part : partitions) {
                    nodes.push_back("/dev/" + part);
                }
            } else {
                // No partition table: the whole disk may carry a filesystem.
                nodes.push_back("/dev/" + disk);
            }
        }
        closedir(dir);
        std::sort(nodes.begin(), nodes.end());
        return nodes;
    }

    bool device_present(const std::string& device_node) override {
        const size_t slash = device_node.rfind('/');
        const std::string name =
            slash == std::string::npos ? device_node : device_node.substr(slash + 1);
        std::string disk = name;
        while (!disk.empty() && std::isdigit(static_cast<unsigned char>(disk.back()))) {
            disk.pop_back();
        }
        if (disk.empty() || disk == name) {
            return is_directory("/sys/block/" + name);
        }
        return is_directory("/sys/block/" + disk + "/" + name);
    }

    bool mount(const std::string& device_node, const std::string& mount_point,
               const std::string& fs_type, const std::string& options) override {
        // Split VFS-generic flags out of the option string, util-linux style:
        // mount(2) takes ro/noatime as flags, the filesystem parser gets the
        // charset options as data.
        unsigned long flags = MS_RDONLY;
        std::string data;
        size_t start = 0;
        while (start <= options.size()) {
            size_t comma = options.find(',', start);
            const std::string token = options.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            if (token == "ro") {
                flags |= MS_RDONLY;
            } else if (token == "noatime") {
                flags |= MS_NOATIME;
            } else {
                if (!data.empty()) {
                    data += ',';
                }
                data += token;
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }

        if (::mount(device_node.c_str(), mount_point.c_str(), fs_type.c_str(), flags,
                    data.empty() ? nullptr : data.c_str()) == 0) {
            return true;
        }
        spdlog::debug("[UsbAutomount] mount({}, {}, {}) failed: {}", device_node, fs_type, options,
                      strerror(errno));
        return false;
    }

    bool unmount(const std::string& mount_point, bool lazy) override {
        if (::umount2(mount_point.c_str(), lazy ? MNT_DETACH : 0) == 0) {
            return true;
        }
        spdlog::debug("[UsbAutomount] umount2({}{}) failed: {}", mount_point, lazy ? ", lazy" : "",
                      strerror(errno));
        return false;
    }

    bool ensure_mount_point_dir(const std::string& mount_point) override {
        // mkdir each path segment; EEXIST is success.
        std::string partial;
        size_t start = 0;
        while (start < mount_point.size()) {
            size_t slash = mount_point.find('/', start);
            if (slash == std::string::npos) {
                slash = mount_point.size();
            }
            if (slash > start) {
                partial += "/" + mount_point.substr(start, slash - start);
                if (mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) {
                    spdlog::debug("[UsbAutomount] mkdir({}) failed: {}", partial, strerror(errno));
                    return false;
                }
            }
            start = slash + 1;
        }
        return is_directory(mount_point);
    }
};

} // namespace

std::unique_ptr<UsbAutomount> UsbAutomount::create() {
    const char* disable = ::getenv("HELIX_USB_AUTOMOUNT");
    if (disable != nullptr && disable[0] == '0' && disable[1] == '\0') {
        spdlog::debug("[UsbAutomount] Disabled by HELIX_USB_AUTOMOUNT=0");
        return nullptr;
    }

    auto ops = std::make_unique<SystemMountOps>();
    if (!ops->is_root()) {
        // The normal desktop case: a developer's build must never mount the
        // workstation's own drives.
        spdlog::debug("[UsbAutomount] Not running as root - fallback mounting off");
        return nullptr;
    }
    // Probed once here, never per poll pass: the answer is a boot-time
    // property of the system, and the ctor bakes it into the grace period.
    return std::make_unique<UsbAutomount>(std::move(ops), kDefaultGrace, probe_primary_mounter());
}

UsbAutomount::UsbAutomount(std::unique_ptr<MountOps> ops, std::chrono::milliseconds grace_period,
                           MounterPresence primary_mounter)
    : ops_(std::move(ops)), grace_(grace_period), armed_(ops_ != nullptr && ops_->is_root()) {
    // The grace exists to lose the race against a primary mounter; with none
    // on the system it is dead time before the first mount. Only a positive
    // "absent" collapses it - every less-certain answer waits it out.
    if (primary_mounter == MounterPresence::ABSENT) {
        grace_ = std::chrono::milliseconds::zero();
    }
}

void UsbAutomount::poll(std::chrono::steady_clock::time_point now) {
    if (!armed_) {
        return;
    }

    std::set<std::string> mounted;
    for (const auto& device : ops_->mounted_devices()) {
        mounted.insert(device);
    }

    // Reap our mounts first: unmount when the device is gone (a yanked stick
    // leaves a stale entry in the mount table), forget when it was unmounted
    // from outside.
    for (auto it = ours_.begin(); it != ours_.end();) {
        if (!ops_->device_present(it->first)) {
            spdlog::info("[UsbAutomount] Device {} vanished - unmounting {}", it->first,
                         it->second);
            unmount_one(it->first, it->second);
            it = ours_.erase(it);
        } else if (mounted.count(it->first) == 0) {
            it = ours_.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& device : ops_->removable_block_devices()) {
        if (ours_.count(device) > 0) {
            continue;
        }
        // A device the mount table lists anywhere belongs to whoever mounted
        // it (udisks2, a vendor app, a previous run of ours): leave it alone.
        if (mounted.count(device) > 0) {
            pending_.erase(device);
            continue;
        }

        auto [it, inserted] = pending_.try_emplace(device);
        if (inserted) {
            it->second.first_seen = now;
        }
        // Grace period: let a primary mounter win the race before mounting.
        if (now - it->second.first_seen < grace_) {
            continue;
        }
        // Cooldown after a failed ladder keeps a hopeless device off the
        // syscall path instead of retrying every poll.
        if (now < it->second.retry_after) {
            continue;
        }
        attempt_mount(device, now);
    }

    // Candidates that vanished mid-grace must not keep a stale first_seen.
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (mounted.count(it->first) == 0 && !ops_->device_present(it->first)) {
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

void UsbAutomount::attempt_mount(const std::string& device_node,
                                 std::chrono::steady_clock::time_point now) {
    const std::string mount_point = automount_mount_point(device_node);
    if (!ops_->ensure_mount_point_dir(mount_point)) {
        pending_[device_node].retry_after = now + kRetryCooldown;
        return;
    }

    auto ladder = automount_ladder();
    // Try what worked before first: the winning combination is a property of
    // this kernel's NLS configuration, not of the stick.
    if (ladder_cache_) {
        auto cached = std::find_if(ladder.begin(), ladder.end(), [this](const MountAttempt& a) {
            return a.fs_type == ladder_cache_->fs_type && a.options == ladder_cache_->options;
        });
        if (cached != ladder.end() && cached != ladder.begin()) {
            std::rotate(ladder.begin(), cached, cached + 1);
        }
    }

    for (const auto& attempt : ladder) {
        if (ops_->mount(device_node, mount_point, attempt.fs_type, attempt.options)) {
            ours_[device_node] = mount_point;
            // Drop the grace state with it: a device that returns as a
            // candidate after an external unmount gets a fresh grace period,
            // never a remount off the stale first sighting.
            pending_.erase(device_node);
            ladder_cache_ = attempt;
            // Field-diagnostic breadcrumb: the (fs, options) pair is what
            // varies across boards and what support will ask for.
            spdlog::info("[UsbAutomount] Mounted {} at {} read-only ({}, options: {})", device_node,
                         mount_point, attempt.fs_type, attempt.options);
            return;
        }
    }

    // Every candidate failed - benign (no recognizable filesystem, charset
    // matrix unsupported). Debug level: not actionable on the device.
    spdlog::debug("[UsbAutomount] No mount option combination worked for {}", device_node);
    pending_[device_node].retry_after = now + kRetryCooldown;
}

void UsbAutomount::unmount_one(const std::string& device_node, const std::string& mount_point) {
    (void)device_node;
    if (ops_->unmount(mount_point, false)) {
        return;
    }
    // EBUSY and friends: a lazy detach is safe for a read-only mount and
    // returns immediately.
    if (ops_->unmount(mount_point, true)) {
        spdlog::debug("[UsbAutomount] Lazy-unmounted {}", mount_point);
    }
}

void UsbAutomount::unmount_all() {
    if (!armed_) {
        return;
    }
    for (const auto& [device, mount_point] : ours_) {
        spdlog::info("[UsbAutomount] Shutting down - unmounting {}", mount_point);
        unmount_one(device, mount_point);
    }
    ours_.clear();
}

} // namespace helix::usb

#endif // __linux__ && !__ANDROID__
