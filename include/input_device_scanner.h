// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace helix {
struct AbsCapabilities;
} // namespace helix

namespace helix::input {

struct ScannedDevice {
    std::string path;
    std::string name;
    int event_num = -1;
};

/// USB HID device with vendor/product identification for manual scanner selection.
struct UsbHidDevice {
    std::string name;       // e.g., "TMS HIDKeyBoard"
    std::string vendor_id;  // e.g., "1a2c" (hex from sysfs)
    std::string product_id; // e.g., "4c5e" (hex from sysfs)
    std::string event_path; // e.g., "/dev/input/event5"
    int bus_type = 0;       // BUS_USB=0x03, BUS_BLUETOOTH=0x05
};

/// Map a uname(2) machine string to the word width in bits of sysfs capability
/// bitmaps, which is the kernel's unsigned long width: 64-bit kernels print
/// 64-bit words, 32-bit kernels print 32-bit words, regardless of how many hex
/// digits any single word happens to show.
int sysfs_bitmap_word_bits_for_machine(const std::string& machine);

/// Word width in bits of sysfs capability bitmaps on the running kernel,
/// derived from uname(2). Falls back to the build's unsigned long width if
/// uname fails.
int sysfs_bitmap_word_bits();

/// Check if a specific bit is set in a sysfs capability hex bitmask whose
/// words are `word_bits` wide. The kernel prints space-separated hex words
/// (one unsigned long each) highest word first, with %lx (no zero padding),
/// and strips leading zero words, so the width cannot be recovered from the
/// string itself and must come from the kernel arch.
bool check_capability_bit_at_width(const std::string& hex_bitmask, int bit, int word_bits);

/// Check if a specific bit is set in a sysfs capability hex bitmask, parsed at
/// the running kernel's word width.
bool check_capability_bit(const std::string& hex_bitmask, int bit);

/// Check whether a device's sysfs VID/PID matches any entry in a blacklist.
/// Each blacklist entry is a "vid:pid" string; both sides of the comparison are
/// normalized (lowercased, whitespace-trimmed, leading "0x" stripped) so that
/// e.g. "1A2C:0003" matches sysfs vendor "1a2c" / product "0003". Malformed
/// entries (missing colon) are skipped. Returns false for an empty blacklist or
/// empty vendor/product.
bool is_vid_pid_blacklisted(const std::string& vendor, const std::string& product,
                            const std::vector<std::string>& blacklist);

/// Read the "input/device_blacklist" config key (a JSON array of "vid:pid"
/// strings) directly from Config. Safe to call during very early startup —
/// swallows all exceptions and returns an empty vector if Config is not yet
/// initialized or the key is absent/malformed.
std::vector<std::string> read_device_blacklist_from_config();

/// Scan /dev/input/event* for mouse devices (REL_X + REL_Y + BTN_LEFT, no ABS_X/ABS_Y).
std::optional<ScannedDevice> find_mouse_device();
std::optional<ScannedDevice> find_mouse_device(const std::string& dev_base,
                                               const std::string& sysfs_base);

/// Scan /dev/input/event* for keyboard devices (KEY_A set).
/// Skips devices that look like barcode scanners ("barcode"/"scanner" in name).
/// If exclude_vendor_product is non-empty (format "vendor:product"), also skips
/// that specific device. Any device whose VID:PID is in `blacklist` is skipped
/// entirely. This prevents LVGL from claiming a barcode scanner as its keyboard
/// input. The no-arg overload builds the blacklist from Config
/// ("input/device_blacklist").
std::optional<ScannedDevice> find_keyboard_device();
std::optional<ScannedDevice> find_keyboard_device(const std::string& dev_base,
                                                  const std::string& sysfs_base,
                                                  const std::string& exclude_vendor_product = "",
                                                  const std::vector<std::string>& blacklist = {});

/// Scan /dev/input/event* for USB HID keyboard-like devices suitable for barcode scanning.
/// Returns ALL matching devices (USB/BT bus, has KEY_A, not a touchscreen).
/// Prioritizes devices with "barcode"/"scanner" in name, then any other HID keyboard.
/// Devices whose VID:PID is in `blacklist` are omitted. The no-arg overload builds
/// the blacklist from Config ("input/device_blacklist").
std::vector<ScannedDevice> find_hid_keyboard_devices();
std::vector<ScannedDevice>
find_hid_keyboard_devices(const std::string& dev_base, const std::string& sysfs_base,
                          const std::vector<std::string>& blacklist = {});

/// Like find_hid_keyboard_devices(), but if configured_vendor_product is non-empty
/// (format "vendor:product", e.g. "1a2c:4c5e"), the matching device is returned
/// as the sole result with highest priority. Falls back to name-based priority
/// if the configured device is not found. Devices whose VID:PID is in `blacklist`
/// are omitted (the configured device is not force-included if blacklisted).
std::vector<ScannedDevice>
find_hid_keyboard_devices(const std::string& dev_base, const std::string& sysfs_base,
                          const std::string& configured_vendor_product,
                          const std::vector<std::string>& blacklist = {});

/// Find the evdev node for a paired Bluetooth HID device by MAC address.
/// Matches /sys/class/input/eventN/device/uniq (case-insensitive) on a
/// BUS_BLUETOOTH device with KEY_A capability. Returns nullopt if not found.
std::optional<ScannedDevice> find_bt_hid_device_by_mac(const std::string& mac);
std::optional<ScannedDevice> find_bt_hid_device_by_mac(const std::string& dev_base,
                                                       const std::string& sysfs_base,
                                                       const std::string& mac);

/// Enumerate all USB HID keyboard-capable devices with vendor/product IDs.
/// Used by the scanner picker UI. Returns all matching devices (no prioritization).
std::vector<UsbHidDevice> enumerate_usb_hid_devices();
std::vector<UsbHidDevice> enumerate_usb_hid_devices(const std::string& dev_base,
                                                    const std::string& sysfs_base);

/// Read a single line from a sysfs file (returns empty string on failure)
std::string read_sysfs_line(const std::string& path);

/// Get device name from /sys/class/input/eventN/device/name
std::string get_input_device_name(int event_num);

/// Get device phys from /sys/class/input/eventN/device/phys
std::string get_input_device_phys(int event_num);

/// Check if input device has touch ABS capabilities, optionally fill AbsCapabilities
bool get_input_touch_capabilities(int event_num, helix::AbsCapabilities* caps_out = nullptr);

} // namespace helix::input
