// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "usb_backend_linux.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

#if defined(__linux__) && !defined(__ANDROID__)

namespace {

// A device whose /sys/block/<name> node never exists on any host, so
// is_usb_mount() reaches its filesystem+mount-point decision deterministically
// instead of depending on this machine's disks.
constexpr const char* kSysfsFreeDevice = "/dev/hxusbtest1";

} // namespace

TEST_CASE("UsbBackendLinux::is_usb_mount accepts every filesystem a stick mounts as",
          "[usb_backend][linux]") {
    UsbBackendLinux backend;

    // msdos is what the kernel registers a FAT mount without long-filename
    // support as; auto-mounters on the printer firmware produce it.
    for (const char* fs : {"vfat", "msdos", "exfat", "ntfs", "ntfs3", "ext4", "ext3", "fuseblk"}) {
        CAPTURE(fs);
        REQUIRE(backend.is_usb_mount(kSysfsFreeDevice, "/media/USBDISK", fs));
    }
}

TEST_CASE("UsbBackendLinux::is_usb_mount rejects non-USB filesystems", "[usb_backend][linux]") {
    UsbBackendLinux backend;

    // iso9660 excluded: a loop-mounted ISO image under /media would pass the
    // /media fallback and surface a disk image as a removable drive.
    // f2fs excluded: internal-flash filesystem on the boards themselves, with
    // no removable use observed.
    for (const char* fs : {"iso9660", "f2fs", "tmpfs", "squashfs", "overlay", "ext2"}) {
        CAPTURE(fs);
        REQUIRE_FALSE(backend.is_usb_mount(kSysfsFreeDevice, "/media/USBDISK", fs));
    }
}

TEST_CASE("UsbBackendLinux::is_usb_mount requires a block device on a media mount point",
          "[usb_backend][linux]") {
    UsbBackendLinux backend;

    // Not a /dev block device
    REQUIRE_FALSE(backend.is_usb_mount("tmpfs", "/media/thing", "vfat"));
    // Mount points that are not removable-media locations
    REQUIRE_FALSE(backend.is_usb_mount(kSysfsFreeDevice, "/", "vfat"));
    REQUIRE_FALSE(backend.is_usb_mount(kSysfsFreeDevice, "/home/user/stick", "ext4"));
    REQUIRE_FALSE(backend.is_usb_mount(kSysfsFreeDevice, "/opt/data", "exfat"));
}

TEST_CASE("UsbBackendLinux monitor starts in mountinfo event mode and stops promptly",
          "[usb_backend][linux]") {
    UsbBackendLinux backend;

    REQUIRE(backend.start().success());
    REQUIRE(backend.is_running());
    // start() silently falling back to content compare on a host where
    // /proc/self/mountinfo is pollable is exactly the regression this guards.
    REQUIRE(backend.using_mountinfo_events());

    std::vector<UsbDrive> drives;
    REQUIRE(backend.get_connected_drives(drives).success());

    // The monitor must honour stop_requested_ within one poll timeout, not hang
    // on an event wait; a loop that blocked indefinitely would hang this test.
    auto t0 = std::chrono::steady_clock::now();
    backend.stop();
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    REQUIRE_FALSE(backend.is_running());
    REQUIRE(elapsed_ms < 5000);
}

TEST_CASE("UsbBackendLinux start and stop are idempotent", "[usb_backend][linux]") {
    UsbBackendLinux backend;

    REQUIRE(backend.start().success());
    REQUIRE(backend.start().success());
    REQUIRE(backend.is_running());

    backend.stop();
    backend.stop();
    REQUIRE_FALSE(backend.is_running());

    // Restart after a stop must re-open the mountinfo fd and re-select the mode.
    REQUIRE(backend.start().success());
    REQUIRE(backend.using_mountinfo_events());
    backend.stop();
}

TEST_CASE("UsbBackendLinux::scan_directory lists every printable extension",
          "[usb_backend][linux]") {
    namespace fs = std::filesystem;

    struct TempDir {
        std::string path;

        TempDir()
            : path((fs::temp_directory_path() /
                    ("usb-scan-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
                       .string()) {
            fs::create_directories(path + "/sub");
        }
        ~TempDir() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } dir;

    auto touch = [&dir](const std::string& name) {
        std::ofstream out(dir.path + "/" + name, std::ios::trunc);
        out << ";";
    };

    touch("benchy.gcode");
    // What a no-LFN FAT mount shows for 3DBenchy.gcode
    touch("3DBENC~1.GCO");
    touch("job.g");
    touch("plate.3mf");
    touch("sub/nested.gco");
    touch("notes.txt");
    touch("firmware.bin");
    touch("package.ufp");

    UsbBackendLinux backend;
    std::vector<UsbGcodeFile> files;
    backend.scan_directory(dir.path, files, 0, 3);

    std::vector<std::string> names;
    for (const auto& f : files) {
        names.push_back(f.filename);
    }
    std::sort(names.begin(), names.end());

    std::vector<std::string> expected = {"3DBENC~1.GCO", "benchy.gcode", "job.g", "nested.gco",
                                         "plate.3mf"};
    std::sort(expected.begin(), expected.end());
    REQUIRE(names == expected);
}

#endif // __linux__ && !__ANDROID__
