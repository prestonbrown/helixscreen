// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "usb_printer_detector.h"

#include "ui_update_queue.h"

#include "http_executor.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <tuple>

#ifdef HELIX_HAS_LIBUSB
#include <libusb.h>
#endif
#include <lvgl.h>

namespace helix {

// ============================================================================
// Known printer table
// ============================================================================

static const std::vector<KnownUsbPrinter> s_known_printers = {
    {0x0493, 0x8760, "Phomemo M110"},
    // Future printers added here
};

const std::vector<KnownUsbPrinter>& UsbPrinterDetector::known_printers() {
    return s_known_printers;
}

bool UsbPrinterDetector::is_known_printer(uint16_t vid, uint16_t pid) {
    for (const auto& p : s_known_printers) {
        if (p.vid == vid && p.pid == pid) {
            return true;
        }
    }
    return false;
}

std::string UsbPrinterDetector::get_printer_name(uint16_t vid, uint16_t pid) {
    for (const auto& p : s_known_printers) {
        if (p.vid == vid && p.pid == pid) {
            return p.name;
        }
    }
    return {};
}

// ============================================================================
// Construction / destruction
// ============================================================================

UsbPrinterDetector::UsbPrinterDetector() = default;

UsbPrinterDetector::~UsbPrinterDetector() {
    stop_polling();
}

// ============================================================================
// Scanning
// ============================================================================

std::vector<UsbPrinterInfo> UsbPrinterDetector::scan() {
    std::vector<UsbPrinterInfo> results;

#ifndef HELIX_HAS_LIBUSB
    spdlog::debug("usb-detect: libusb not available on this platform");
    return results;
#else
    libusb_context* ctx = nullptr;
    int rc = libusb_init(&ctx);
    if (rc != 0) {
        spdlog::warn("usb-detect: libusb_init failed: {}",
                     libusb_strerror(static_cast<libusb_error>(rc)));
        return results;
    }

    libusb_device** device_list = nullptr;
    ssize_t count = libusb_get_device_list(ctx, &device_list);
    if (count < 0) {
        spdlog::warn("usb-detect: libusb_get_device_list failed: {}",
                     libusb_strerror(static_cast<libusb_error>(count)));
        libusb_exit(ctx);
        return results;
    }

    for (ssize_t i = 0; i < count; i++) {
        libusb_device* dev = device_list[i];
        struct libusb_device_descriptor desc {};
        if (libusb_get_device_descriptor(dev, &desc) != 0) {
            continue;
        }

        if (!is_known_printer(desc.idVendor, desc.idProduct)) {
            continue;
        }

        UsbPrinterInfo info;
        info.vid = desc.idVendor;
        info.pid = desc.idProduct;
        info.bus = libusb_get_bus_number(dev);
        info.address = libusb_get_device_address(dev);
        info.product_name = get_printer_name(desc.idVendor, desc.idProduct);

        // Try to read serial string descriptor (requires opening the device)
        if (desc.iSerialNumber != 0) {
            libusb_device_handle* handle = nullptr;
            if (libusb_open(dev, &handle) == 0) {
                unsigned char buf[256] = {};
                int len = libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber, buf,
                                                             sizeof(buf));
                if (len > 0) {
                    info.serial.assign(reinterpret_cast<char*>(buf), static_cast<size_t>(len));
                }
                libusb_close(handle);
            }
        }

        spdlog::debug("usb-detect: found {} (VID:{:04x} PID:{:04x}) bus:{} addr:{} serial:{}",
                      info.product_name, info.vid, info.pid, info.bus, info.address, info.serial);

        results.push_back(std::move(info));
    }

    libusb_free_device_list(device_list, 1);
    libusb_exit(ctx);

    return results;
#endif // HELIX_HAS_LIBUSB
}

// ============================================================================
// Polling
// ============================================================================

void UsbPrinterDetector::poll_timer_cb(_lv_timer_t* timer) {
    auto* self = static_cast<UsbPrinterDetector*>(lv_timer_get_user_data(timer));
    if (self && self->callback_) {
        self->request_scan();
    }
}

void UsbPrinterDetector::scan_async(ScanCallback on_done) {
    auto& executor = helix::http::HttpExecutor::fast();
    if (!executor.running()) {
        on_done(scan());
        return;
    }
    executor.submit([on_done = std::move(on_done)]() {
        auto detected = scan();
        helix::ui::queue_update(
            "UsbPrinterDetector::scan",
            [on_done, detected = std::move(detected)]() mutable { on_done(std::move(detected)); });
    });
}

void UsbPrinterDetector::request_scan() {
    if (scan_in_flight_) {
        return;
    }
    scan_in_flight_ = true;
    scan_async([this, tok = poll_lifetime_.token()](std::vector<UsbPrinterInfo> detected) {
        if (tok.expired()) {
            return;
        }
        scan_in_flight_ = false;
        apply_scan(std::move(detected));
    });
}

void UsbPrinterDetector::apply_scan(std::vector<UsbPrinterInfo> detected) {
    if (!callback_) {
        return;
    }
    // Always fire on first scan (so "Searching..." updates to "No printers found"),
    // then only fire on subsequent changes
    if (first_scan_ || !results_equal(detected, last_detected_)) {
        first_scan_ = false;
        last_detected_ = detected;
        auto cb = callback_;
        cb(detected);
    }
}

void UsbPrinterDetector::start_polling(DetectionCallback callback, int interval_ms) {
    stop_polling();

    callback_ = std::move(callback);
    last_detected_.clear();
    first_scan_ = true;

    poll_timer_ = lv_timer_create([](lv_timer_t* t) { poll_timer_cb(t); },
                                  static_cast<uint32_t>(interval_ms), this);

    spdlog::debug("usb-detect: started polling every {}ms", interval_ms);

    // Scan now rather than one interval from now
    request_scan();
}

void UsbPrinterDetector::stop_polling() {
    if (poll_timer_) {
        lv_timer_delete(poll_timer_);
        poll_timer_ = nullptr;
        spdlog::debug("usb-detect: stopped polling");
    }
    callback_ = nullptr;
}

bool UsbPrinterDetector::is_polling() const {
    return poll_timer_ != nullptr;
}

bool UsbPrinterDetector::results_equal(std::vector<UsbPrinterInfo> a,
                                       std::vector<UsbPrinterInfo> b) {
    if (a.size() != b.size()) {
        return false;
    }
    auto cmp = [](const UsbPrinterInfo& x, const UsbPrinterInfo& y) {
        return std::tie(x.bus, x.address) < std::tie(y.bus, y.address);
    };
    std::sort(a.begin(), a.end(), cmp);
    std::sort(b.begin(), b.end(), cmp);
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].vid != b[i].vid || a[i].pid != b[i].pid || a[i].bus != b[i].bus ||
            a[i].address != b[i].address) {
            return false;
        }
    }
    return true;
}

} // namespace helix
