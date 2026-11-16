// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace helix {

/// Hardware keymap layout the USB scanner is configured to emit.
///
/// HID barcode scanners translate their internal ASCII output into evdev
/// keycodes according to a layout programmed into the scanner itself. If that
/// layout doesn't match what we assume on the host side, characters get
/// mis-mapped (e.g. AZERTY 'a' arrives as evdev KEY_Q). The user must tell us
/// which layout the scanner is using — it cannot be inferred from the app
/// language or anything else in settings.
enum class ScannerKeymap {
    Qwerty, ///< US QWERTY (default). Correct for most scanners sold worldwide.
    Qwertz, ///< German QWERTZ. Y and Z swapped vs QWERTY.
    Azerty, ///< French AZERTY. A/Q, Z/W, M/; swapped; shifted digit row.
};

class UsbScannerMonitor {
  public:
    // Callback fires on the monitor background thread.
    // Callers must use ui_queue_update() if touching LVGL objects.
    using ScanCallback = std::function<void(int spool_id)>;

    UsbScannerMonitor() = default;
    ~UsbScannerMonitor();

    UsbScannerMonitor(const UsbScannerMonitor&) = delete;
    UsbScannerMonitor& operator=(const UsbScannerMonitor&) = delete;

    void start(ScanCallback on_scan);
    void stop();
    bool is_running() const {
        return running_.load();
    }

    /// Convert evdev keycode to ASCII for the given hardware keymap.
    ///
    /// Returns 0 for keycodes that don't produce a character in that layout.
    /// The layout parameter defaults to Qwerty to preserve the pre-keymap
    /// public API for existing tests and call sites.
    static char keycode_to_char(int keycode, bool shift,
                                ScannerKeymap layout = ScannerKeymap::Qwerty);

    /// Parse a setting string ("qwerty"|"qwertz"|"azerty") into the enum.
    /// Unknown values fall back to Qwerty.
    static ScannerKeymap parse_keymap(const std::string& s);

    /// Push a new keymap layout into the currently running monitor instance,
    /// if any. Lets the settings UI apply a layout change live without needing
    /// to stop/restart the monitor thread or cycle the scanner overlay. No-op
    /// when no monitor is running.
    ///
    /// THREADING: LVGL/UI thread only. Safety relies on the owner of the
    /// running UsbScannerMonitor (currently QrScannerOverlay) being destroyed
    /// on the same thread as callers of this method, which serializes the
    /// s_live_instance_ load against teardown. Do not call from background
    /// callbacks (WebSocket, HTTP, timers) — that reintroduces a UAF window
    /// between the pointer load and the layout store.
    static void set_active_layout(ScannerKeymap layout);

    /// True when a scanner configured in Settings > Barcode Scanner is attached,
    /// so spool scanning can skip the camera. Only configured devices count: an
    /// empty id or MAC is never present, whatever its presence flag says.
    static bool use_configured_scanner(const std::string& usb_id, const std::string& bt_mac,
                                       bool usb_present, bool bt_present);

    /// Probes /dev/input and /sys for the configured USB VID:PID and paired BT
    /// MAC, then applies use_configured_scanner(). An unconfigured setup or an
    /// auto-detected keyboard returns false.
    static bool configured_scanner_present();

    // Public for testing: check if accumulated string is a Spoolman QR code
    // and extract the spool ID. Returns -1 if not a match.
    static int check_spoolman_pattern(const std::string& input);

  private:
    void monitor_thread_func();
    struct ScannerSource {
        std::string path;
        bool grab; ///< true = EVIOCGRAB exclusively (BT scanner paired here);
                   ///< false = passive read (USB; preserves caps-lock LED workflow).
    };
    std::vector<ScannerSource> find_scanner_devices();

    std::thread monitor_thread_;
    std::atomic<bool> running_{false};
    int stop_pipe_[2] = {-1, -1};
    ScanCallback callback_;
    std::atomic<ScannerKeymap> layout_{ScannerKeymap::Qwerty};

    /// Live pointer to the one running monitor instance so the settings UI
    /// can push layout updates without plumbing an instance handle through
    /// the overlay/modal hierarchy. Only one scanner runs at a time.
    /// Written/read from the LVGL/UI thread only — see set_active_layout().
    static std::atomic<UsbScannerMonitor*> s_live_instance_;
};

} // namespace helix
