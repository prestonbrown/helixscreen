// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct _lv_timer_t;

namespace helix {

/// Information about a detected USB printer
struct UsbPrinterInfo {
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string serial;
    std::string product_name;
    uint8_t bus = 0;
    uint8_t address = 0;
};

/// Entry in the known USB printer table
struct KnownUsbPrinter {
    uint16_t vid;
    uint16_t pid;
    std::string name;
};

/**
 * @brief Detects known USB label printers via libusb
 *
 * Scans the USB bus for devices matching known VID:PID pairs.
 * Supports one-shot scanning and periodic polling with change detection.
 *
 * Polling uses an LVGL timer, so start/stop must be called from the
 * LVGL thread. Each poll enumerates the bus on HttpExecutor::fast() (libusb
 * opens every matching device, too slow for a frame) and the detection
 * callback runs on the UI thread with the result.
 */
class UsbPrinterDetector {
  public:
    UsbPrinterDetector();
    ~UsbPrinterDetector();

    // Non-copyable
    UsbPrinterDetector(const UsbPrinterDetector&) = delete;
    UsbPrinterDetector& operator=(const UsbPrinterDetector&) = delete;

    using DetectionCallback = std::function<void(const std::vector<UsbPrinterInfo>&)>;

    /// Scan once for known USB printers (synchronous; blocks on libusb)
    static std::vector<UsbPrinterInfo> scan();

    using ScanCallback = std::function<void(std::vector<UsbPrinterInfo>)>;

    /// Scan once on HttpExecutor::fast(); @p on_done runs on the UI thread.
    /// Runs inline when the executor is not running.
    static void scan_async(ScanCallback on_done);

    /// Start periodic scanning. Callback fires on the UI thread.
    void start_polling(DetectionCallback callback, int interval_ms = 3000);

    /// Stop periodic scanning
    void stop_polling();

    /// Whether periodic polling is active
    [[nodiscard]] bool is_polling() const;

    /// A bus scan is running or its result has not been applied yet (UI thread).
    [[nodiscard]] bool is_scanning() const {
        return scan_in_flight_;
    }

    /// Known printer VID:PID table
    static const std::vector<KnownUsbPrinter>& known_printers();

    /// Check if a VID:PID is in the known table
    static bool is_known_printer(uint16_t vid, uint16_t pid);

    /// Get name for a known VID:PID (empty if unknown)
    static std::string get_printer_name(uint16_t vid, uint16_t pid);

  private:
    static void poll_timer_cb(_lv_timer_t* timer);

    /// Submit a bus scan unless one is still running (UI thread).
    void request_scan();

    /// Fire the callback on the first result or a changed one (UI thread).
    void apply_scan(std::vector<UsbPrinterInfo> detected);

    /// Compare two result sets by VID+PID+bus+address
    static bool results_equal(std::vector<UsbPrinterInfo> a, std::vector<UsbPrinterInfo> b);

    _lv_timer_t* poll_timer_ = nullptr;
    DetectionCallback callback_;
    std::vector<UsbPrinterInfo> last_detected_;
    bool first_scan_ = true;
    bool scan_in_flight_ = false;

    /// A scan landing after this detector is destroyed is dropped. One that
    /// lands after stop_polling() finds no callback and does nothing.
    helix::AsyncLifetimeGuard poll_lifetime_;
};

} // namespace helix
