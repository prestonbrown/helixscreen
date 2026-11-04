// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "phomemo_printer.h"

#include "core/update_queue.h"
#include "helix_thread.h"
#include "phomemo_protocol.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <thread>

namespace helix {

// Phomemo M110 defaults
static constexpr uint8_t kDefaultSpeed = 0x03;
static constexpr uint8_t kDefaultDensity = 0x0A;

PhomemoPrinter::PhomemoPrinter() = default;
PhomemoPrinter::~PhomemoPrinter() = default;

std::string PhomemoPrinter::name() const {
    return "Phomemo M110";
}

void PhomemoPrinter::set_device(uint16_t vid, uint16_t pid, const std::string& serial) {
    vid_ = vid;
    pid_ = pid;
    serial_ = serial;
}

std::vector<LabelSize> PhomemoPrinter::supported_sizes() const {
    return supported_sizes_static();
}

std::vector<LabelSize> PhomemoPrinter::supported_sizes_static() {
    // All sizes at 203 DPI, gap/die-cut media (0x0A)
    return {
        {"40x30mm", 319, 240, 203, 0x0A, 40, 30}, {"40x20mm", 319, 160, 203, 0x0A, 40, 20},
        {"50x30mm", 400, 240, 203, 0x0A, 50, 30}, {"50x50mm", 400, 400, 203, 0x0A, 50, 50},
        {"50x80mm", 400, 640, 203, 0x0A, 50, 80}, {"30x20mm", 240, 160, 203, 0x0A, 30, 20},
        {"25x10mm", 200, 80, 203, 0x0A, 25, 10},  {"40x60mm", 319, 480, 203, 0x0A, 40, 60},
    };
}

std::vector<uint8_t> PhomemoPrinter::build_raster_commands(const LabelBitmap& bitmap,
                                                           const LabelSize& size) {
    return helix::label::phomemo_build_raster(bitmap, size);
}

uint16_t PhomemoPrinter::read_sysfs_usb_id(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open())
        return 0;
    std::string val;
    f >> val;
    return text_io::parse_leading<uint16_t>(val, 16).value_or(0);
}

// Find the /dev/usb/lp* device node for a given VID:PID by checking sysfs
static std::string find_usblp_device(uint16_t vid, uint16_t pid) {
#ifdef __ANDROID__
    (void)vid;
    (void)pid;
    return {}; // USB device nodes not accessible on Android
#else
    // Scan /dev/usb/lp* and match against sysfs VID:PID
    namespace fs = std::filesystem;

    for (int i = 0; i < 8; i++) {
        std::string dev_path = fmt::format("/dev/usb/lp{}", i);
        std::string sysfs_path = fmt::format("/sys/class/usbmisc/lp{}/device/../", i);

        uint16_t dev_vid = PhomemoPrinter::read_sysfs_usb_id(sysfs_path + "idVendor");
        uint16_t dev_pid = PhomemoPrinter::read_sysfs_usb_id(sysfs_path + "idProduct");

        if (dev_vid == vid && dev_pid == pid) {
            spdlog::debug("Phomemo: matched {} to {:04x}:{:04x}", dev_path, vid, pid);
            return dev_path;
        }
    }
    return {};
#endif
}

void PhomemoPrinter::print(const LabelBitmap& bitmap, const LabelSize& size,
                           PrintCallback callback) {
    if (vid_ == 0 || pid_ == 0) {
        spdlog::error("Phomemo: USB device not configured (vid/pid not set)");
        helix::ui::queue_update("PhomemoPrinter::print", [callback]() {
            if (callback)
                callback(false, "USB device not configured");
        });
        return;
    }

    auto commands = build_raster_commands(bitmap, size);
    spdlog::warn("Phomemo: sending {} bytes to USB {:04x}:{:04x}", commands.size(), vid_, pid_);

    uint16_t vid = vid_;
    uint16_t pid = pid_;

    // Wrap thread spawn in try/catch — pthread_create EAGAIN on resource-constrained
    // ARM (AD5M/CC1) throws std::system_error which aborts with std::terminate
    // if it escapes an LVGL event frame (#724, #837, [L083]).
    try {
        helix::make_thread([vid, pid, commands = std::move(commands), callback]() {
            bool success = false;
            std::string error;

            // Find the usblp device node for this VID:PID
            std::string dev_path = find_usblp_device(vid, pid);
            if (dev_path.empty()) {
                error = fmt::format("No USB printer device found for {:04x}:{:04x}. "
                                    "Is the printer turned on?",
                                    vid, pid);
                spdlog::error("Phomemo: {}", error);
            } else {
                std::ofstream f(dev_path, std::ios::binary);
                if (!f.is_open()) {
                    error = fmt::format("Cannot open {} (check permissions)", dev_path);
                    spdlog::error("Phomemo: {}", error);
                } else {
                    f.write(reinterpret_cast<const char*>(commands.data()),
                            static_cast<std::streamsize>(commands.size()));
                    f.flush();
                    if (f.good()) {
                        success = true;
                        spdlog::warn("Phomemo: sent {} bytes via {}", commands.size(), dev_path);
                    } else {
                        error = fmt::format("Write to {} failed", dev_path);
                        spdlog::error("Phomemo: {}", error);
                    }
                }
            }

            helix::ui::queue_update("PhomemoPrinter::print",
                                    [callback, success, error]() { callback(success, error); });
        }).detach();
    } catch (const std::system_error& e) {
        spdlog::error("Phomemo: failed to spawn print thread: {}", e.what());
        helix::ui::queue_update("PhomemoPrinter::print", [callback]() {
            callback(false, "System busy — please try again");
        });
    }
}

} // namespace helix

#endif // HELIX_HAS_LABEL_PRINTER
