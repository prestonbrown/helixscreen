// SPDX-License-Identifier: GPL-3.0-or-later

#include "input_device_scanner.h"

#include "config.h"
#include "log_redact.h"
#include "touch_calibration.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

namespace {

// Build sysfs path: /sys/class/input/eventN/device/<subpath>
std::string sysfs_device_path(const std::string& sysfs_base, int event_num,
                              const std::string& subpath) {
    return sysfs_base + "/event" + std::to_string(event_num) + "/device/" + subpath;
}

std::string read_sysfs_capability(const std::string& sysfs_base, int event_num,
                                  const std::string& cap_name) {
    return helix::input::read_sysfs_line(
        sysfs_device_path(sysfs_base, event_num, "capabilities/" + cap_name));
}

std::string read_device_name(const std::string& sysfs_base, int event_num) {
    return helix::input::read_sysfs_line(sysfs_device_path(sysfs_base, event_num, "name"));
}

// Read /sys/class/input/eventN/device/id/bustype — returns bus type as integer.
// BUS_USB=3, BUS_BLUETOOTH=5. Returns 0 on failure.
int read_bus_type(const std::string& sysfs_base, int event_num) {
    std::string line =
        helix::input::read_sysfs_line(sysfs_device_path(sysfs_base, event_num, "id/bustype"));
    if (line.empty())
        return 0;
    return static_cast<int>(std::strtol(line.c_str(), nullptr, 16));
}

std::string read_vendor_id(const std::string& sysfs_base, int event_num) {
    return helix::input::read_sysfs_line(sysfs_device_path(sysfs_base, event_num, "id/vendor"));
}

std::string read_product_id(const std::string& sysfs_base, int event_num) {
    return helix::input::read_sysfs_line(sysfs_device_path(sysfs_base, event_num, "id/product"));
}

constexpr int BUS_USB = 0x03;
constexpr int BUS_BLUETOOTH = 0x05;

// Normalize a hex USB VID or PID token for comparison: strip surrounding
// whitespace, drop a leading "0x"/"0X", and lowercase. So "  0x1A2C " -> "1a2c".
std::string normalize_hex_id(const std::string& s) {
    size_t start = 0;
    size_t end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;
    std::string out = s.substr(start, end - start);
    if (out.size() >= 2 && out[0] == '0' && (out[1] == 'x' || out[1] == 'X'))
        out = out.substr(2);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

} // namespace

namespace helix::input {

int sysfs_bitmap_word_bits_for_machine(const std::string& machine) {
    // x86_64, aarch64, mips64, riscv64, loongarch64, ppc64/ppc64le, sparc64
    if (machine.find("64") != std::string::npos)
        return 64;
    // A 32-bit process on an aarch64 kernel sees COMPAT_UTS_MACHINE "armv8l";
    // the kernel that printed the bitmap is still 64-bit.
    if (machine.rfind("armv8", 0) == 0)
        return 64;
    if (machine == "s390x")
        return 64;
    // i386..i686, armv6l, armv7l, mips, ppc, ...
    return 32;
}

int sysfs_bitmap_word_bits() {
    struct utsname uts;
    if (uname(&uts) != 0)
        return static_cast<int>(sizeof(unsigned long) * 8);
    return sysfs_bitmap_word_bits_for_machine(uts.machine);
}

bool check_capability_bit_at_width(const std::string& hex_bitmask, int bit, int word_bits) {
    if (bit < 0 || hex_bitmask.empty() || word_bits <= 0) {
        return false;
    }

    // Split on spaces into words (rightmost = lowest bits)
    std::vector<std::string> words;
    std::istringstream stream(hex_bitmask);
    std::string word;
    while (stream >> word) {
        words.push_back(word);
    }

    if (words.empty()) {
        return false;
    }

    // Determine which word contains our bit (right-to-left, 0-indexed)
    int word_index_from_right = bit / word_bits;
    int bit_in_word = bit % word_bits;

    // Convert to array index (words[0] = leftmost = highest bits)
    int array_index = static_cast<int>(words.size()) - 1 - word_index_from_right;
    if (array_index < 0 || array_index >= static_cast<int>(words.size())) {
        return false;
    }

    // uint64_t holds a full 64-bit word even when built for a 32-bit host
    // (armv8l userland on an aarch64 kernel), where the plain 1UL shift and
    // strtoul would overflow on bit 63 / 16-digit words.
    uint64_t val = std::strtoull(words[array_index].c_str(), nullptr, 16);
    return (val & (uint64_t(1) << bit_in_word)) != 0;
}

bool check_capability_bit(const std::string& hex_bitmask, int bit) {
    return check_capability_bit_at_width(hex_bitmask, bit, sysfs_bitmap_word_bits());
}

bool is_vid_pid_blacklisted(const std::string& vendor, const std::string& product,
                            const std::vector<std::string>& blacklist) {
    if (blacklist.empty() || vendor.empty() || product.empty())
        return false;

    std::string want_vendor = normalize_hex_id(vendor);
    std::string want_product = normalize_hex_id(product);

    for (const auto& entry : blacklist) {
        auto colon = entry.find(':');
        if (colon == std::string::npos)
            continue; // malformed entry — skip gracefully
        std::string entry_vendor = normalize_hex_id(entry.substr(0, colon));
        std::string entry_product = normalize_hex_id(entry.substr(colon + 1));
        if (entry_vendor.empty() || entry_product.empty())
            continue;
        if (entry_vendor == want_vendor && entry_product == want_product)
            return true;
    }
    return false;
}

std::vector<std::string> read_device_blacklist_from_config() {
    // Read directly from Config (not SettingsManager) because find_keyboard_device()
    // runs during display backend init, before SettingsManager::init_subjects().
    try {
        Config* config = Config::get_instance();
        if (config) {
            // Top-level "/input/..." path (not df()/per-printer) to match the
            // template placement and sibling keys like /input/touch_device.
            return config->get<std::vector<std::string>>("/input/device_blacklist", {});
        }
    } catch (...) {
        // Config may not be initialized yet during very early startup, or the key
        // may hold an unexpected type. Treat as no blacklist.
    }
    return {};
}

std::optional<ScannedDevice> find_mouse_device(const std::string& dev_base,
                                               const std::string& sysfs_base) {
    auto dir = std::unique_ptr<DIR, decltype(&closedir)>(opendir(dev_base.c_str()), closedir);
    if (!dir) {
        spdlog::debug("[InputScanner] Cannot open {}", dev_base);
        return std::nullopt;
    }

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        int event_num = -1;
        if (sscanf(entry->d_name, "event%d", &event_num) != 1 || event_num < 0) {
            continue;
        }

        std::string device_path = dev_base + "/" + entry->d_name;
        if (access(device_path.c_str(), R_OK) != 0) {
            continue;
        }

        std::string name = read_device_name(sysfs_base, event_num);
        std::string abs_caps = read_sysfs_capability(sysfs_base, event_num, "abs");
        std::string rel_caps = read_sysfs_capability(sysfs_base, event_num, "rel");
        std::string key_caps = read_sysfs_capability(sysfs_base, event_num, "key");

        spdlog::debug("[InputScanner] Scanning {} ({}) abs=[{}] rel=[{}] key=[{}]", device_path,
                      name, abs_caps, rel_caps, key_caps);

        // Skip touchscreens:
        //   - Legacy single-touch: ABS_X (bit 0) + ABS_Y (bit 1)
        //   - MT-only (e.g. Goodix gt9xxnew_ts): ABS_MT_POSITION_X (bit 53) +
        //     ABS_MT_POSITION_Y (bit 54) without legacy ABS_X/ABS_Y
        //   - Any device with BTN_TOUCH (bit 330) — touchscreens, not mice
        bool has_legacy_abs =
            check_capability_bit(abs_caps, 0) && check_capability_bit(abs_caps, 1);
        bool has_mt_abs = check_capability_bit(abs_caps, 53) && check_capability_bit(abs_caps, 54);
        bool has_btn_touch = check_capability_bit(key_caps, 330);

        if (has_legacy_abs || has_mt_abs || has_btn_touch) {
            spdlog::debug("[InputScanner] Skipping {} (touchscreen: legacy_abs={} mt_abs={} "
                          "btn_touch={})",
                          device_path, has_legacy_abs, has_mt_abs, has_btn_touch);
            continue;
        }

        // Require REL_X (bit 0) + REL_Y (bit 1)
        if (!check_capability_bit(rel_caps, 0) || !check_capability_bit(rel_caps, 1)) {
            spdlog::debug("[InputScanner] Skipping {} (no REL_X/REL_Y)", device_path);
            continue;
        }

        // Require BTN_LEFT (bit 272)
        if (!check_capability_bit(key_caps, 272)) {
            spdlog::debug("[InputScanner] Skipping {} (no BTN_LEFT)", device_path);
            continue;
        }

        // Only accept USB or Bluetooth devices — excludes SoC-integrated IR
        // receivers (e.g. MCE IR Keyboard/Mouse on Allwinner), HDMI CEC virtual
        // devices, and other non-physical "mice" that report mouse capabilities.
        int bus = read_bus_type(sysfs_base, event_num);
        if (bus != BUS_USB && bus != BUS_BLUETOOTH) {
            spdlog::debug("[InputScanner] Skipping {} (bus type 0x{:04x}, not USB/BT)", device_path,
                          bus);
            continue;
        }

        spdlog::info("[InputScanner] Found mouse: {} ({})", device_path, name);
        return ScannedDevice{device_path, name, event_num};
    }

    return std::nullopt;
}

std::optional<ScannedDevice> find_mouse_device() {
    return find_mouse_device("/dev/input", "/sys/class/input");
}

std::optional<ScannedDevice> find_keyboard_device(const std::string& dev_base,
                                                  const std::string& sysfs_base,
                                                  const std::string& exclude_vendor_product,
                                                  const std::vector<std::string>& blacklist) {
    // Parse exclusion vendor:product pair if provided
    std::string exclude_vendor;
    std::string exclude_product;
    if (!exclude_vendor_product.empty()) {
        auto colon = exclude_vendor_product.find(':');
        if (colon != std::string::npos) {
            exclude_vendor = exclude_vendor_product.substr(0, colon);
            exclude_product = exclude_vendor_product.substr(colon + 1);
        }
    }

    auto dir = std::unique_ptr<DIR, decltype(&closedir)>(opendir(dev_base.c_str()), closedir);
    if (!dir) {
        spdlog::debug("[InputScanner] Cannot open {}", dev_base);
        return std::nullopt;
    }

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        int event_num = -1;
        if (sscanf(entry->d_name, "event%d", &event_num) != 1 || event_num < 0) {
            continue;
        }

        std::string device_path = dev_base + "/" + entry->d_name;
        if (access(device_path.c_str(), R_OK) != 0) {
            continue;
        }

        // Only accept USB or Bluetooth devices
        int bus = read_bus_type(sysfs_base, event_num);
        if (bus != BUS_USB && bus != BUS_BLUETOOTH) {
            continue;
        }

        // Require KEY_A (bit 30) — distinguishes real keyboards from power buttons etc.
        std::string key_caps = read_sysfs_capability(sysfs_base, event_num, "key");
        if (!check_capability_bit(key_caps, 30)) {
            continue;
        }

        std::string name = read_device_name(sysfs_base, event_num);

        // Skip devices whose VID:PID is on the user's blacklist. This is the
        // general escape hatch for generically-named scanners that slip past the
        // name/VID:PID heuristics below.
        if (!blacklist.empty()) {
            std::string vendor = read_vendor_id(sysfs_base, event_num);
            std::string product = read_product_id(sysfs_base, event_num);
            if (is_vid_pid_blacklisted(vendor, product, blacklist)) {
                spdlog::info("[InputScanner] Skipping blacklisted device for keyboard: {} ({}) "
                             "vendor={} product={}",
                             device_path, name, vendor, product);
                continue;
            }
        }

        // Skip devices with "barcode" or "scanner" in the name — these are
        // barcode scanners that happen to present as USB HID keyboards.
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower_name.find("barcode") != std::string::npos ||
            lower_name.find("scanner") != std::string::npos) {
            spdlog::info("[InputScanner] Skipping scanner device for keyboard: {} ({})",
                         device_path, name);
            continue;
        }

        // Skip device matching the user's configured scanner vendor:product
        if (!exclude_vendor.empty()) {
            std::string vendor = read_vendor_id(sysfs_base, event_num);
            std::string product = read_product_id(sysfs_base, event_num);
            if (vendor == exclude_vendor && product == exclude_product) {
                spdlog::info("[InputScanner] Skipping configured scanner for keyboard: {} ({})",
                             device_path, name);
                continue;
            }
        }

        spdlog::info("[InputScanner] Found keyboard: {} ({})", device_path, name);
        return ScannedDevice{device_path, name, event_num};
    }

    return std::nullopt;
}

std::optional<ScannedDevice> find_keyboard_device() {
    // Read scanner device ID directly from Config to exclude it from keyboard
    // detection. We use Config instead of SettingsManager because this runs
    // during display backend init, before SettingsManager::init_subjects().
    std::string exclude_id;
    try {
        Config* config = Config::get_instance();
        if (config) {
            exclude_id = config->get<std::string>("/scanner/usb_vendor_product", "");
        }
    } catch (...) {
        // Config may not be initialized yet during very early startup
    }
    return find_keyboard_device("/dev/input", "/sys/class/input", exclude_id,
                                read_device_blacklist_from_config());
}

std::vector<ScannedDevice> find_hid_keyboard_devices(const std::string& dev_base,
                                                     const std::string& sysfs_base,
                                                     const std::vector<std::string>& blacklist) {
    std::vector<ScannedDevice> named_scanners;    // "barcode"/"scanner" in name — high priority
    std::vector<ScannedDevice> generic_keyboards; // any other USB HID keyboard

    auto dir = std::unique_ptr<DIR, decltype(&closedir)>(opendir(dev_base.c_str()), closedir);
    if (!dir) {
        spdlog::debug("[InputScanner] Cannot open {}", dev_base);
        return {};
    }

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        int event_num = -1;
        if (sscanf(entry->d_name, "event%d", &event_num) != 1 || event_num < 0) {
            continue;
        }

        std::string device_path = dev_base + "/" + entry->d_name;
        if (access(device_path.c_str(), R_OK) != 0) {
            continue;
        }

        // Only accept USB or Bluetooth devices
        int bus = read_bus_type(sysfs_base, event_num);
        if (bus != BUS_USB && bus != BUS_BLUETOOTH) {
            continue;
        }

        // Require KEY_A (bit 30) — real keyboard-like device
        std::string key_caps = read_sysfs_capability(sysfs_base, event_num, "key");
        if (!check_capability_bit(key_caps, 30)) {
            continue;
        }

        // Skip touchscreens (have ABS_X/ABS_Y or ABS_MT_POSITION_X/Y)
        std::string abs_caps = read_sysfs_capability(sysfs_base, event_num, "abs");
        bool has_legacy_abs =
            check_capability_bit(abs_caps, 0) && check_capability_bit(abs_caps, 1);
        bool has_mt_abs = check_capability_bit(abs_caps, 53) && check_capability_bit(abs_caps, 54);
        if (has_legacy_abs || has_mt_abs) {
            continue;
        }

        // Skip devices whose VID:PID is on the user's blacklist.
        if (!blacklist.empty()) {
            std::string vendor = read_vendor_id(sysfs_base, event_num);
            std::string product = read_product_id(sysfs_base, event_num);
            if (is_vid_pid_blacklisted(vendor, product, blacklist)) {
                spdlog::info("[InputScanner] Skipping blacklisted HID device: {} vendor={} "
                             "product={}",
                             device_path, vendor, product);
                continue;
            }
        }

        std::string name = read_device_name(sysfs_base, event_num);
        ScannedDevice dev{device_path, name, event_num};

        // Prioritize devices with "barcode" or "scanner" in the name
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower_name.find("barcode") != std::string::npos ||
            lower_name.find("scanner") != std::string::npos) {
            spdlog::info("[InputScanner] Found named scanner device: {} ({})", device_path, name);
            named_scanners.push_back(std::move(dev));
        } else {
            spdlog::info("[InputScanner] Found USB HID keyboard device: {} ({})", device_path,
                         name);
            generic_keyboards.push_back(std::move(dev));
        }
    }

    // Return named scanners first, then generic keyboards
    std::vector<ScannedDevice> result;
    result.reserve(named_scanners.size() + generic_keyboards.size());
    for (auto& d : named_scanners)
        result.push_back(std::move(d));
    for (auto& d : generic_keyboards)
        result.push_back(std::move(d));
    return result;
}

std::vector<ScannedDevice> find_hid_keyboard_devices() {
    return find_hid_keyboard_devices("/dev/input", "/sys/class/input",
                                     read_device_blacklist_from_config());
}

std::vector<ScannedDevice> find_hid_keyboard_devices(const std::string& dev_base,
                                                     const std::string& sysfs_base,
                                                     const std::string& configured_vendor_product,
                                                     const std::vector<std::string>& blacklist) {
    if (configured_vendor_product.empty()) {
        return find_hid_keyboard_devices(dev_base, sysfs_base, blacklist);
    }

    // Parse "vendor:product" string
    auto colon = configured_vendor_product.find(':');
    if (colon == std::string::npos) {
        spdlog::warn("[InputScanner] Invalid configured_vendor_product format: {}",
                     configured_vendor_product);
        return find_hid_keyboard_devices(dev_base, sysfs_base, blacklist);
    }
    std::string target_vendor = configured_vendor_product.substr(0, colon);
    std::string target_product = configured_vendor_product.substr(colon + 1);

    // Scan for the configured device
    auto dir = std::unique_ptr<DIR, decltype(&closedir)>(opendir(dev_base.c_str()), closedir);
    if (!dir)
        return find_hid_keyboard_devices(dev_base, sysfs_base, blacklist);

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;

        int event_num = -1;
        if (sscanf(entry->d_name, "event%d", &event_num) != 1 || event_num < 0)
            continue;

        std::string device_path = dev_base + "/" + entry->d_name;
        if (access(device_path.c_str(), R_OK) != 0)
            continue;

        int bus = read_bus_type(sysfs_base, event_num);
        if (bus != BUS_USB && bus != BUS_BLUETOOTH)
            continue;

        std::string vendor = read_vendor_id(sysfs_base, event_num);
        std::string product = read_product_id(sysfs_base, event_num);

        // A blacklisted device is never used, even if it matches the configured
        // VID:PID — the blacklist is the stronger signal.
        if (is_vid_pid_blacklisted(vendor, product, blacklist))
            continue;

        if (vendor == target_vendor && product == target_product) {
            std::string name = read_device_name(sysfs_base, event_num);
            spdlog::info("[InputScanner] Found configured scanner device: {} ({}) "
                         "vendor={} product={}",
                         device_path, name, vendor, product);
            return {{device_path, name, event_num}};
        }
    }

    spdlog::info("[InputScanner] Configured device {}:{} not found, falling back to auto-detect",
                 target_vendor, target_product);
    return find_hid_keyboard_devices(dev_base, sysfs_base, blacklist);
}

static std::string normalize_mac(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ':' || c == '-' || c == ' ')
            continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::optional<ScannedDevice> find_bt_hid_device_by_mac(const std::string& dev_base,
                                                       const std::string& sysfs_base,
                                                       const std::string& mac) {
    if (mac.empty())
        return std::nullopt;

    std::string mac_norm = normalize_mac(mac);

    auto dir = std::unique_ptr<DIR, decltype(&closedir)>(opendir(dev_base.c_str()), closedir);
    if (!dir)
        return std::nullopt;

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;

        int event_num = -1;
        if (sscanf(entry->d_name, "event%d", &event_num) != 1 || event_num < 0)
            continue;

        std::string device_path = dev_base + "/" + entry->d_name;
        if (access(device_path.c_str(), R_OK) != 0)
            continue;

        if (read_bus_type(sysfs_base, event_num) != BUS_BLUETOOTH)
            continue;

        std::string uniq = helix::input::read_sysfs_line(
            sysfs_base + "/event" + std::to_string(event_num) + "/device/uniq");
        if (normalize_mac(uniq) != mac_norm)
            continue;

        std::string key_caps = read_sysfs_capability(sysfs_base, event_num, "key");
        if (!check_capability_bit(key_caps, 30))
            continue;

        std::string name = read_device_name(sysfs_base, event_num);
        spdlog::info("[InputScanner] Found paired BT HID scanner: {} ({}) mac={}", device_path,
                     name, helix::redact::mac(mac));
        return ScannedDevice{device_path, name, event_num};
    }

    spdlog::debug("[InputScanner] No BT HID evdev found for mac={}", helix::redact::mac(mac));
    return std::nullopt;
}

std::optional<ScannedDevice> find_bt_hid_device_by_mac(const std::string& mac) {
    return find_bt_hid_device_by_mac("/dev/input", "/sys/class/input", mac);
}

std::string read_sysfs_line(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open())
        return "";
    std::string line;
    std::getline(file, line);
    return line;
}

std::string get_input_device_name(int event_num) {
    std::string path = "/sys/class/input/event" + std::to_string(event_num) + "/device/name";
    return read_sysfs_line(path);
}

std::string get_input_device_phys(int event_num) {
    std::string path = "/sys/class/input/event" + std::to_string(event_num) + "/device/phys";
    return read_sysfs_line(path);
}

std::vector<UsbHidDevice> enumerate_usb_hid_devices(const std::string& dev_base,
                                                    const std::string& sysfs_base) {
    std::vector<UsbHidDevice> devices;

    auto dir = std::unique_ptr<DIR, decltype(&closedir)>(opendir(dev_base.c_str()), closedir);
    if (!dir) {
        spdlog::debug("[InputScanner] Cannot open {}", dev_base);
        return devices;
    }

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;

        int event_num = -1;
        if (sscanf(entry->d_name, "event%d", &event_num) != 1 || event_num < 0)
            continue;

        std::string device_path = dev_base + "/" + entry->d_name;
        if (access(device_path.c_str(), R_OK) != 0)
            continue;

        int bus = read_bus_type(sysfs_base, event_num);
        if (bus != BUS_USB && bus != BUS_BLUETOOTH)
            continue;

        std::string key_caps = read_sysfs_capability(sysfs_base, event_num, "key");
        if (!check_capability_bit(key_caps, 30))
            continue;

        std::string abs_caps = read_sysfs_capability(sysfs_base, event_num, "abs");
        bool has_legacy_abs =
            check_capability_bit(abs_caps, 0) && check_capability_bit(abs_caps, 1);
        bool has_mt_abs = check_capability_bit(abs_caps, 53) && check_capability_bit(abs_caps, 54);
        if (has_legacy_abs || has_mt_abs)
            continue;

        std::string name = read_device_name(sysfs_base, event_num);
        std::string vendor = read_vendor_id(sysfs_base, event_num);
        std::string product = read_product_id(sysfs_base, event_num);

        spdlog::info("[InputScanner] Enumerated USB HID device: {} ({}) vendor={} product={}",
                     device_path, name, vendor, product);
        devices.push_back(
            {std::move(name), std::move(vendor), std::move(product), std::move(device_path), bus});
    }

    return devices;
}

std::vector<UsbHidDevice> enumerate_usb_hid_devices() {
    return enumerate_usb_hid_devices("/dev/input", "/sys/class/input");
}

bool get_input_touch_capabilities(int event_num, helix::AbsCapabilities* caps_out) {
    std::string path =
        "/sys/class/input/event" + std::to_string(event_num) + "/device/capabilities/abs";
    std::string caps = read_sysfs_line(path);
    if (caps.empty())
        return false;

    auto result = helix::parse_abs_capabilities(caps);
    if (caps_out)
        *caps_out = result;

    if (result.has_multitouch && !result.has_single_touch) {
        spdlog::debug("[InputScanner] event{}: MT-only touchscreen detected "
                      "(no legacy ABS_X/ABS_Y)",
                      event_num);
    }

    return result.has_single_touch || result.has_multitouch;
}

} // namespace helix::input
