// SPDX-License-Identifier: GPL-3.0-or-later
#include "usb_scanner_monitor.h"

#include "input_device_scanner.h"
#include "log_redact.h"
#include "qr_decoder.h"
#include "settings_manager.h"

#include <spdlog/spdlog.h>

#ifdef __linux__
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace helix {

std::atomic<UsbScannerMonitor*> UsbScannerMonitor::s_live_instance_{nullptr};

void UsbScannerMonitor::set_active_layout(ScannerKeymap layout) {
    UsbScannerMonitor* inst = s_live_instance_.load(std::memory_order_acquire);
    if (inst) {
        inst->layout_.store(layout, std::memory_order_relaxed);
    }
}

UsbScannerMonitor::~UsbScannerMonitor() {
    stop();
}

namespace {

struct LetterEntry {
    int keycode;
    char lower;
};

// US QWERTY physical-layout letter mapping (matches standard Linux input keymap).
constexpr LetterEntry QWERTY_LETTERS[] = {
    {16, 'q'}, {17, 'w'}, {18, 'e'}, {19, 'r'}, {20, 't'}, {21, 'y'}, {22, 'u'},
    {23, 'i'}, {24, 'o'}, {25, 'p'}, {30, 'a'}, {31, 's'}, {32, 'd'}, {33, 'f'},
    {34, 'g'}, {35, 'h'}, {36, 'j'}, {37, 'k'}, {38, 'l'}, {44, 'z'}, {45, 'x'},
    {46, 'c'}, {47, 'v'}, {48, 'b'}, {49, 'n'}, {50, 'm'},
};

// German QWERTZ: identical to QWERTY except physical Y and Z are swapped
// (keycodes 21 and 44). Punctuation differs in some spots on real German
// keyboards but we leave it at QWERTY mappings here — Spoolman QR payloads
// use only ASCII letters/digits/`-`/`+`/`:` and the scanner produces those via
// shifted/unshifted letter/digit keys, which ARE covered.
// NOTE: QWERTZ punctuation mapping is incomplete by design — covers Spoolman
// URL decoding only, not general-purpose German text entry.
constexpr LetterEntry QWERTZ_LETTERS[] = {
    {16, 'q'}, {17, 'w'}, {18, 'e'}, {19, 'r'}, {20, 't'}, {21, 'z'}, {22, 'u'},
    {23, 'i'}, {24, 'o'}, {25, 'p'}, {30, 'a'}, {31, 's'}, {32, 'd'}, {33, 'f'},
    {34, 'g'}, {35, 'h'}, {36, 'j'}, {37, 'k'}, {38, 'l'}, {44, 'y'}, {45, 'x'},
    {46, 'c'}, {47, 'v'}, {48, 'b'}, {49, 'n'}, {50, 'm'},
};

// French AZERTY: A/Q swapped (16<->30), Z/W swapped (17<->44), M moves from
// keycode 50 to keycode 39 (where QWERTY has ';'); keycode 50 becomes ','.
// NOTE: AZERTY support is partial — covers Spoolman URL decoding, not
// general-purpose French text entry. Accented/dead keys are not handled.
constexpr LetterEntry AZERTY_LETTERS[] = {
    {16, 'a'}, {17, 'z'}, {18, 'e'}, {19, 'r'}, {20, 't'}, {21, 'y'}, {22, 'u'},
    {23, 'i'}, {24, 'o'}, {25, 'p'}, {30, 'q'}, {31, 's'}, {32, 'd'}, {33, 'f'},
    {34, 'g'}, {35, 'h'}, {36, 'j'}, {37, 'k'}, {38, 'l'}, {39, 'm'}, {44, 'w'},
    {45, 'x'}, {46, 'c'}, {47, 'v'}, {48, 'b'}, {49, 'n'},
    // keycode 50 on AZERTY is ',' (see punctuation section of keycode_to_char)
};

template <size_t N> char lookup_letter(const LetterEntry (&table)[N], int keycode, bool shift) {
    for (const auto& entry : table) {
        if (entry.keycode == keycode) {
            return shift ? static_cast<char>(std::toupper(entry.lower)) : entry.lower;
        }
    }
    return 0;
}

} // namespace

ScannerKeymap UsbScannerMonitor::parse_keymap(const std::string& s) {
    if (s == "qwertz")
        return ScannerKeymap::Qwertz;
    if (s == "azerty")
        return ScannerKeymap::Azerty;
    return ScannerKeymap::Qwerty;
}

char UsbScannerMonitor::keycode_to_char(int keycode, bool shift, ScannerKeymap layout) {
    // Digit row: AZERTY defaults to the shifted form (e.g. unshifted "1" key
    // produces '&', shift+1 produces '1'). QWERTY and QWERTZ share the US
    // digit-row mapping.
    if (layout == ScannerKeymap::Azerty) {
        // keycode 2..11 correspond to the top number row keys 1..0.
        if (keycode >= 2 && keycode <= 11) {
            // Shifted (e.g. shift+1) yields the literal digit.
            if (shift) {
                static const char shifted_digits[] = "1234567890";
                return shifted_digits[keycode - 2];
            }
            // Unshifted AZERTY digit row, keycodes 2..11: & é " ' ( - è _ ç à
            // Only the ASCII-producing positions are handled; accented
            // characters (é è ç à) are dropped — Spoolman URL decoding needs
            // only ASCII, so returning 0 keeps the accumulator parseable.
            switch (keycode) {
            case 2:
                return '&'; // unshifted "1"
            case 4:
                return '"'; // unshifted "3"
            case 5:
                return '\''; // unshifted "4"
            case 6:
                return '('; // unshifted "5"
            case 7:
                return '-'; // unshifted "6" — convenient for Spoolman s-<id>
            case 9:
                return '_'; // unshifted "8"
            case 3:
            case 8:
            case 10:
            case 11:
            default:
                return 0; // é, è, ç, à — non-ASCII, dropped
            }
        }
    } else {
        // QWERTY / QWERTZ digit row.
        if (keycode >= 2 && keycode <= 10) {
            if (shift) {
                static const char shifted_nums[] = "!@#$%^&*(";
                return shifted_nums[keycode - 2];
            }
            return '0' + (keycode - 1);
        }
        if (keycode == 11)
            return shift ? ')' : '0';
    }

    // Keycodes 12 (KEY_MINUS) and 13 (KEY_EQUAL): treated as QWERTY in all
    // layouts. Spoolman barcodes need a literal '-' and the scanner will emit
    // KEY_MINUS for that character regardless of programmed layout if it uses
    // ASCII passthrough, so keeping this stable avoids regressions.
    if (keycode == 12)
        return shift ? '_' : '-';
    if (keycode == 13)
        return shift ? '+' : '=';

    // Letter table dispatch.
    char letter = 0;
    switch (layout) {
    case ScannerKeymap::Qwertz:
        letter = lookup_letter(QWERTZ_LETTERS, keycode, shift);
        break;
    case ScannerKeymap::Azerty:
        letter = lookup_letter(AZERTY_LETTERS, keycode, shift);
        break;
    case ScannerKeymap::Qwerty:
    default:
        letter = lookup_letter(QWERTY_LETTERS, keycode, shift);
        break;
    }
    if (letter != 0)
        return letter;

    // Punctuation. AZERTY remaps several keys vs QWERTY:
    //   39 -> 'm' (handled in AZERTY_LETTERS above)
    //   50 -> ','
    //   51 -> ';' unshifted, '.' shifted (QWERTY: ',' / '<')
    //   52 -> ':' unshifted, '/' shifted (QWERTY: '.' / '>')
    //   53 -> '!' unshifted               (QWERTY: '/' / '?')
    if (layout == ScannerKeymap::Azerty) {
        if (keycode == 50)
            return shift ? '?' : ',';
        if (keycode == 51)
            return shift ? '.' : ';';
        if (keycode == 52)
            return shift ? '/' : ':';
        if (keycode == 53)
            return shift ? 0 : '!'; // '§' shifted — non-ASCII, dropped
        // Keycode 39 on AZERTY is 'm' (handled in AZERTY_LETTERS above).
    } else {
        if (keycode == 39)
            return shift ? ':' : ';';
    }

    if (keycode == 40)
        return shift ? '"' : '\'';
    if (keycode == 41)
        return shift ? '~' : '`';
    if (keycode == 43)
        return shift ? '|' : '\\';
    if (keycode == 26)
        return shift ? '{' : '[';
    if (keycode == 27)
        return shift ? '}' : ']';
    if (layout != ScannerKeymap::Azerty) {
        if (keycode == 51)
            return shift ? '<' : ',';
        if (keycode == 52)
            return shift ? '>' : '.';
        if (keycode == 53)
            return shift ? '?' : '/';
    }
    if (keycode == 57)
        return ' '; // KEY_SPACE

    return 0;
}

int UsbScannerMonitor::check_spoolman_pattern(const std::string& input) {
    return QrDecoder::parse_spoolman_id(input);
}

#ifdef __linux__

std::vector<UsbScannerMonitor::ScannerSource> UsbScannerMonitor::find_scanner_devices() {
    // Prefer a paired Bluetooth HID scanner if one is configured. BT HID
    // scanners don't have the caps-lock LED handshake quirk that some USB
    // scanners do, so we can grab them exclusively to prevent keystrokes from
    // leaking into focused UI widgets.
    std::string bt_mac = helix::SettingsManager::instance().get_scanner_bt_address();
    if (!bt_mac.empty()) {
        auto bt_dev = helix::input::find_bt_hid_device_by_mac(bt_mac);
        if (bt_dev) {
            spdlog::info("UsbScannerMonitor: using BT HID scanner: {} ({}) [exclusive grab]",
                         bt_dev->path, bt_dev->name);
            return {ScannerSource{bt_dev->path, true}};
        }
        spdlog::debug("UsbScannerMonitor: paired BT scanner {} not present, falling back to USB",
                      helix::redact::mac(bt_mac));
    }

    // USB fallback. Caller-configured VID:PID disambiguates from regular keyboards.
    // The user's device blacklist ("input/device_blacklist") is also applied so the
    // scan overlay ignores devices HelixScreen is told to leave alone.
    std::string configured_id = helix::SettingsManager::instance().get_scanner_device_id();
    auto blacklist = helix::input::read_device_blacklist_from_config();
    auto devices = helix::input::find_hid_keyboard_devices("/dev/input", "/sys/class/input",
                                                           configured_id, blacklist);
    if (devices.empty())
        return {};

    spdlog::info("UsbScannerMonitor: using HID device: {} ({}) [passive read]", devices[0].path,
                 devices[0].name);
    return {ScannerSource{devices[0].path, false}};
}

void UsbScannerMonitor::start(ScanCallback on_scan) {
    if (running_.load()) {
        spdlog::warn("UsbScannerMonitor: already running");
        return;
    }

    callback_ = std::move(on_scan);

    // Load scanner keymap layout from settings. Scanners emit evdev keycodes
    // according to a layout programmed into the scanner hardware; without the
    // right layout selected here, characters get mis-mapped (e.g. an AZERTY
    // scanner producing 'q' when the barcode actually contains 'a').
    // TODO: optionally seed default from app language.
    std::string keymap_str = helix::SettingsManager::instance().get_scanner_keymap();
    layout_.store(parse_keymap(keymap_str), std::memory_order_relaxed);
    spdlog::info("UsbScannerMonitor: keymap layout: {}", keymap_str);

    s_live_instance_.store(this, std::memory_order_release);

    if (pipe(stop_pipe_) != 0) {
        spdlog::error("UsbScannerMonitor: failed to create pipe: {}", strerror(errno));
        return;
    }

    running_.store(true);
    monitor_thread_ = std::thread(&UsbScannerMonitor::monitor_thread_func, this);
    spdlog::info("UsbScannerMonitor: started");
}

void UsbScannerMonitor::stop() {
    if (!running_.load())
        return;

    // Mark the thread as winding down before unpublishing the live pointer, so
    // the invariant "s_live_instance_ == this ⟹ monitor thread is running"
    // always holds from the UI thread's perspective.
    running_.store(false);

    UsbScannerMonitor* expected = this;
    s_live_instance_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);

    if (stop_pipe_[1] >= 0) {
        char c = 'x';
        (void)write(stop_pipe_[1], &c, 1);
    }

    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }

    if (stop_pipe_[0] >= 0) {
        close(stop_pipe_[0]);
        stop_pipe_[0] = -1;
    }
    if (stop_pipe_[1] >= 0) {
        close(stop_pipe_[1]);
        stop_pipe_[1] = -1;
    }

    spdlog::info("UsbScannerMonitor: stopped");
}

void UsbScannerMonitor::monitor_thread_func() {
    spdlog::debug("UsbScannerMonitor: monitor thread started");

    std::vector<int> device_fds;
    std::string accumulator;
    std::string keycode_trace; // raw keycodes for diagnostics
    bool capslock_on = false;
    bool shift_held = false;
    std::chrono::steady_clock::time_point last_key_time{};

    auto open_devices = [&]() {
        for (int fd : device_fds)
            close(fd);
        device_fds.clear();
        capslock_on = false;
        shift_held = false;
        accumulator.clear();
        keycode_trace.clear();

        auto sources = find_scanner_devices();
        for (const auto& src : sources) {
            int fd = open(src.path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd < 0) {
                spdlog::warn("UsbScannerMonitor: failed to open {}: {}", src.path, strerror(errno));
                continue;
            }
            // EVIOCGRAB only on BT HID scanners. USB scanners may rely on the
            // kernel's `leds` handler responding to scanner-emitted Caps Lock
            // toggles (a few quirky USB scanners stall waiting for the LED
            // ack); BT HID scanners don't do that handshake, so grabbing them
            // is safe and prevents keystrokes from leaking into focused
            // widgets.
            if (src.grab) {
                if (ioctl(fd, EVIOCGRAB, 1) != 0) {
                    spdlog::warn("UsbScannerMonitor: EVIOCGRAB failed on {}: {} "
                                 "(keystrokes will leak)",
                                 src.path, strerror(errno));
                } else {
                    spdlog::info("UsbScannerMonitor: exclusive grab on {}", src.path);
                }
            }
            device_fds.push_back(fd);
        }
    };

    open_devices();

    while (running_.load()) {
        // Build poll fd list: stop_pipe + device fds
        std::vector<struct pollfd> pfds;
        pfds.push_back({stop_pipe_[0], POLLIN, 0});

        if (device_fds.empty()) {
            int ret = poll(pfds.data(), pfds.size(), 5000);
            if (ret > 0 && (pfds[0].revents & POLLIN))
                break;
            open_devices();
            continue;
        }

        for (int fd : device_fds) {
            pfds.push_back({fd, POLLIN, 0});
        }

        int ret = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 5000);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            spdlog::error("UsbScannerMonitor: poll error: {}", strerror(errno));
            break;
        }
        if (ret == 0)
            continue;
        if (pfds[0].revents & POLLIN)
            break;

        // Clear stale partial scans (e.g. interrupted mid-barcode)
        if (!accumulator.empty()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_key_time);
            if (elapsed.count() >= 3) {
                spdlog::debug("UsbScannerMonitor: clearing stale accumulator ({}s idle)",
                              elapsed.count());
                accumulator.clear();
                keycode_trace.clear();
            }
        }

        bool device_error = false;
        for (size_t i = 1; i < pfds.size(); ++i) {
            if (!(pfds[i].revents & POLLIN))
                continue;

            // Drain all available events — scanners send rapid bursts
            while (true) {
                struct input_event ev {};
                ssize_t n = read(pfds[i].fd, &ev, sizeof(ev));
                if (n < static_cast<ssize_t>(sizeof(ev))) {
                    if (n < 0 && errno != EAGAIN) {
                        spdlog::warn("UsbScannerMonitor: device read error, will re-scan");
                        device_error = true;
                    }
                    break;
                }

                if (ev.type != EV_KEY)
                    continue;

                // Track Caps Lock — scanners toggle it for uppercase letters.
                if (ev.code == KEY_CAPSLOCK) {
                    if (ev.value == 1)
                        capslock_on = !capslock_on;
                    continue;
                }

                if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
                    shift_held = (ev.value != 0);
                    continue;
                }

                if (ev.value != 1)
                    continue;

                last_key_time = std::chrono::steady_clock::now();

                if (ev.code == KEY_ENTER || ev.code == KEY_KPENTER) {
                    if (!accumulator.empty()) {
                        auto layout = layout_.load(std::memory_order_relaxed);
                        const char* layout_name = layout == ScannerKeymap::Qwertz   ? "qwertz"
                                                  : layout == ScannerKeymap::Azerty ? "azerty"
                                                                                    : "qwerty";
                        spdlog::info("UsbScannerMonitor: scan complete: "
                                     "text='{}' keycodes=[{}] layout={}",
                                     accumulator, keycode_trace, layout_name);
                        int spool_id = check_spoolman_pattern(accumulator);
                        if (spool_id >= 0 && callback_) {
                            spdlog::info("UsbScannerMonitor: detected Spoolman spool ID: {}",
                                         spool_id);
                            callback_(spool_id);
                        } else if (spool_id < 0) {
                            spdlog::info("UsbScannerMonitor: text '{}' not a Spoolman QR code",
                                         accumulator);
                        }
                        accumulator.clear();
                        keycode_trace.clear();
                    }
                    continue;
                }

                // Caps Lock affects only letter keys; Shift affects everything.
                int code = static_cast<int>(ev.code);
                bool is_letter = (code >= 16 && code <= 25) || // Q-P
                                 (code >= 30 && code <= 38) || // A-L
                                 (code >= 44 && code <= 50);   // Z-M
                bool effective_shift = is_letter ? (shift_held ^ capslock_on) : shift_held;
                char ch =
                    keycode_to_char(code, effective_shift, layout_.load(std::memory_order_relaxed));
                if (!keycode_trace.empty())
                    keycode_trace += ' ';
                keycode_trace += std::to_string(code);
                if (effective_shift)
                    keycode_trace += 'S';
                spdlog::debug("UsbScannerMonitor: keycode={} shift={} caps={} -> '{}'", code,
                              effective_shift, capslock_on, ch ? std::string(1, ch) : "(none)");
                if (ch != 0) {
                    accumulator += ch;
                }
            }
        }

        if (device_error) {
            open_devices();
        }
    }

    for (int fd : device_fds)
        close(fd);

    spdlog::debug("UsbScannerMonitor: monitor thread exiting");
}

#else // !__linux__

std::vector<UsbScannerMonitor::ScannerSource> UsbScannerMonitor::find_scanner_devices() {
    return {};
}
void UsbScannerMonitor::start(ScanCallback) {
    spdlog::warn("UsbScannerMonitor: only supported on Linux");
}
void UsbScannerMonitor::stop() {}
void UsbScannerMonitor::monitor_thread_func() {}

#endif // __linux__

} // namespace helix
