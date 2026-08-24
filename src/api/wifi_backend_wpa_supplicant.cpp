// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_backend_wpa_supplicant.h"

#include "ui_error_reporting.h"

#include "log_redact.h"
#include "spdlog/fmt/fmt.h"
#include "spdlog/spdlog.h"
#include "wifi_5ghz_detection.h"
#include "wifi_saved_config.h"

// find_network_id() is pure string parsing with no OS dependency, and its
// declaration in the header sits outside the __APPLE__ guard so it can be
// unit-tested on macOS — so its definition lives outside the guard too,
// unlike the wpa_ctrl-dependent implementation below.
namespace helix::wifi::detail {
std::string find_network_id(const std::string& list_networks_reply, const std::string& ssid) {
    if (list_networks_reply.empty() || ssid.empty())
        return {};

    // Skip the header line ("network id / ssid / bssid / flags").
    size_t line_start = list_networks_reply.find('\n');
    if (line_start == std::string::npos)
        return {};
    line_start += 1;

    while (line_start < list_networks_reply.size()) {
        const size_t line_end = list_networks_reply.find('\n', line_start);
        const size_t line_len =
            (line_end == std::string::npos) ? std::string::npos : line_end - line_start;
        const std::string line = list_networks_reply.substr(line_start, line_len);

        const size_t tab1 = line.find('\t');
        if (tab1 != std::string::npos) {
            const size_t tab2 = line.find('\t', tab1 + 1);
            const std::string this_ssid = (tab2 == std::string::npos)
                                              ? line.substr(tab1 + 1)
                                              : line.substr(tab1 + 1, tab2 - tab1 - 1);
            if (this_ssid == ssid)
                return line.substr(0, tab1);
        }

        if (line_end == std::string::npos)
            break;
        line_start = line_end + 1;
    }
    return {};
}

// Character/length rules for a value that will be spliced into a
// wpa_supplicant SET_NETWORK command. Pure predicate, no logging — shared by
// validate_wpa_string() below (which additionally logs *what* failed, for
// non-secret fields like an SSID) and reconcile_saved_networks() (which must
// validate a stored PSK without ever routing it through a diagnostic that
// echoes the offending byte). Declared in the header outside the __APPLE__
// guard, like find_network_id, so it can be unit-tested directly instead of
// only indirectly through platform-specific backend behaviour.
bool wpa_string_is_valid(const std::string& input) {
    for (char c : input) {
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' || c < 32 || c == 127)
            return false;
    }
    return !input.empty() && input.length() <= 255;
}

// Thin wrapper over wpa_string_is_valid(): one rule set, two behaviours. Logs
// the specific violation on failure, so this must only be called with values
// safe to echo into a log line — an SSID, never a PSK or other secret.
// reconcile_saved_networks() validates a stored PSK via wpa_string_is_valid()
// directly instead of this function, for exactly that reason.
std::string validate_wpa_string(const std::string& input, const std::string& field_name) {
    if (wpa_string_is_valid(input))
        return input;

    if (input.empty() || input.length() > 255) {
        LOG_ERROR_INTERNAL("Invalid {} length: {}", field_name, input.length());
    } else {
        LOG_ERROR_INTERNAL("Invalid character in {}", field_name);
    }
    return "";
}
} // namespace helix::wifi::detail

#if !defined(__APPLE__) && !defined(__ANDROID__)
// ============================================================================
// Linux Implementation: Full wpa_supplicant integration
// ============================================================================

#include "wpa_ctrl.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

namespace fs = std::filesystem;

/// Resolve a client socket directory visible outside systemd's PrivateTmp.
///
/// wpa_ctrl_open() binds a UNIX datagram reply socket in /tmp by default.
/// Under PrivateTmp=true the real /tmp is a private mount — wpa_supplicant
/// (running outside the namespace) cannot sendto() the reply address, so
/// wpa_ctrl_attach() blocks until timeout.
///
/// We prefer /run/helixscreen (created by RuntimeDirectory=helixscreen in the
/// service unit).  Falls back to /tmp for manual/non-systemd launches.
static const char* resolve_wpa_client_dir() {
    // RuntimeDirectory is the ideal location — visible to all processes
    if (fs::is_directory("/run/helixscreen"))
        return "/run/helixscreen";
    return nullptr; // nullptr → wpa_ctrl_open() uses its /tmp default
}

/// Normalise a wpa_supplicant control-interface value to a plain directory.
///
/// The `-O` / `ctrl_interface` value is either a bare directory
/// (e.g. Creality's `/etc/wifi/wpa_supplicant/sockets`) or the keyed form
/// `DIR=<path> GROUP=<grp>` (the standard systemd launch).  Return the <path>.
static std::string parse_wpa_ctrl_value(std::string v) {
    const std::string key = "DIR=";
    const auto pos = v.find(key);
    if (pos != std::string::npos)
        v.erase(0, pos + key.size());
    // Drop a trailing " GROUP=..." (or any other whitespace-separated token).
    const auto ws = v.find_first_of(" \t");
    if (ws != std::string::npos)
        v.erase(ws);
    return v;
}

/// Parse a wpa_supplicant config file's `ctrl_interface=` directive.
///
/// Vendor init scripts frequently launch `wpa_supplicant -c <conf>` with a
/// non-standard ctrl_interface set in the config file rather than passing
/// -O/-C on the command line, so scanning the cmdline alone would miss them.
/// The value uses the same bare-dir or `DIR=<path> GROUP=<grp>` form as -O.
/// Returns "" if the file is unreadable, has no ctrl_interface, or the value
/// is not an absolute directory path.
namespace helix::wifi::detail {
std::string read_ctrl_interface_from_conf(const std::string& conf_path) {
    std::ifstream conf(conf_path);
    if (!conf.is_open())
        return {};

    const std::string key = "ctrl_interface=";
    std::string line;
    while (std::getline(conf, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#')
            continue; // blank line or comment
        if (line.compare(first, key.size(), key) != 0)
            continue;

        std::string val = line.substr(first + key.size());
        // Trim trailing whitespace / CR (CRLF configs).
        const auto last = val.find_last_not_of(" \t\r\n");
        if (last != std::string::npos)
            val.erase(last + 1);

        std::string dir = parse_wpa_ctrl_value(std::move(val));
        // ctrl_interface may also be a DBus-style name; only accept it when it
        // resolves to an absolute filesystem directory path.
        if (!dir.empty() && dir.front() == '/')
            return dir;
    }
    return {};
}

bool wpa_config_has_network(const std::string& config_contents, const std::string& ssid) {
    if (ssid.empty())
        return false;

    // wpa_supplicant writes `\tssid="name"` inside each network block. Match the
    // quoted form exactly so a partial name ("Home" vs "HomeGuest") can't pass.
    const std::string needle = "ssid=\"" + ssid + "\"";

    size_t pos = 0;
    while ((pos = config_contents.find(needle, pos)) != std::string::npos) {
        const size_t after = pos + needle.size();
        // Reject a prefix match on a longer SSID (ssid="Home" vs ssid="Home2"):
        // the closing quote must be the final char of the token.
        const bool ends_clean = after >= config_contents.size() || config_contents[after] == '\n' ||
                                config_contents[after] == '\r' || config_contents[after] == ' ' ||
                                config_contents[after] == '\t';
        // `bssid=` and `scan_ssid=` end in the same characters, so require the
        // match to start a token rather than continue one.
        const bool starts_clean =
            pos == 0 || config_contents[pos - 1] == '\n' || config_contents[pos - 1] == '\r' ||
            config_contents[pos - 1] == ' ' || config_contents[pos - 1] == '\t';
        if (ends_clean && starts_clean)
            return true;
        pos = after;
    }
    return false;
}

SavePersistence classify_save_result(const std::string& save_reply,
                                     const std::string& config_contents, const std::string& ssid) {
    // A failed reply is unambiguous.
    if (save_reply.compare(0, 2, "OK") != 0)
        return SavePersistence::NotPersisted;

    // "OK" only means the command was accepted. The file is the authority.
    return wpa_config_has_network(config_contents, ssid) ? SavePersistence::Persisted
                                                         : SavePersistence::NotPersisted;
}

RemovalPersistence classify_removal_result(bool conf_path_known, bool conf_readable,
                                           const std::string& conf_contents,
                                           const std::string& ssid) {
    if (!conf_path_known || !conf_readable)
        return RemovalPersistence::Unverifiable;
    return wpa_config_has_network(conf_contents, ssid) ? RemovalPersistence::StillListed
                                                       : RemovalPersistence::Verified;
}

ScanTrigger classify_scan_reply(const std::string& reply) {
    if (reply.empty())
        return ScanTrigger::NoReply;
    if (reply.compare(0, 2, "OK") == 0)
        return ScanTrigger::Started;
    if (reply.compare(0, 9, "FAIL-BUSY") == 0)
        return ScanTrigger::AlreadyBusy;
    return ScanTrigger::Failed;
}
} // namespace helix::wifi::detail

/// Inspect running processes for a `wpa_supplicant` daemon and return the
/// control-interface directory it was launched with.
///
/// The directory is whatever wpa_supplicant was launched with; some vendor
/// firmwares relocate it off the standard paths.  Auto-detecting it from the
/// live process lets us adapt without per-device patches or manual symlinks.
/// Precedence mirrors wpa_supplicant itself: `-O <dir>` overrides the control
/// directory; otherwise the `ctrl_interface` in the `-c <conf>` config file;
/// otherwise a `-C <ctrl>` value (used only when no -c is given).
/// Returns "" when not found (no daemon, no usable arg, or /proc unavailable).
/// Return the `-c <conf>` path of the running wpa_supplicant ("" if none).
///
/// Deliberately a separate walk rather than a shared scan with
/// detect_wpa_ctrl_dir_from_proc(): that function ships on every device and
/// its per-PID precedence/fallthrough is load-bearing, so it is left alone.
static std::string detect_wpa_conf_path_from_proc() {
    std::error_code ec;
    if (!fs::is_directory("/proc", ec))
        return {};

    for (const auto& entry : fs::directory_iterator("/proc", ec)) {
        if (ec)
            break;
        const std::string name = entry.path().filename().string();
        if (name.empty() ||
            !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); }))
            continue;

        std::ifstream cmd(entry.path() / "cmdline", std::ios::binary);
        if (!cmd.is_open())
            continue;
        std::vector<std::string> argv;
        std::string arg;
        while (std::getline(cmd, arg, '\0'))
            argv.push_back(arg);
        if (argv.empty() || fs::path(argv[0]).filename().string() != "wpa_supplicant")
            continue;

        for (size_t i = 1; i < argv.size(); ++i) {
            const std::string& a = argv[i];
            if (a == "-c" && i + 1 < argv.size())
                return argv[i + 1];
            if (a.rfind("-c", 0) == 0 && a.size() > 2)
                return a.substr(2);
        }
    }
    return {};
}

static std::string detect_wpa_ctrl_dir_from_proc() {
    std::error_code ec;
    if (!fs::is_directory("/proc", ec))
        return {};

    for (const auto& entry : fs::directory_iterator("/proc", ec)) {
        if (ec)
            break;
        // PID directories only.
        const std::string name = entry.path().filename().string();
        if (name.empty() ||
            !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); }))
            continue;

        // /proc/<pid>/cmdline is NUL-separated argv.
        std::ifstream cmd(entry.path() / "cmdline", std::ios::binary);
        if (!cmd.is_open())
            continue;
        std::vector<std::string> argv;
        std::string arg;
        while (std::getline(cmd, arg, '\0'))
            argv.push_back(arg);
        if (argv.empty())
            continue;

        // argv[0] basename must be wpa_supplicant.
        if (fs::path(argv[0]).filename().string() != "wpa_supplicant")
            continue;

        // Collect the relevant flags. Each accepts a separate "-X value" or a
        // joined "-Xvalue" form. -O / -C values may be the keyed
        // "DIR=<path> GROUP=<grp>" form (parse_wpa_ctrl_value handles both).
        std::string o_dir, c_val, conf_path;
        for (size_t i = 1; i < argv.size(); ++i) {
            const std::string& a = argv[i];
            if (a == "-O" && i + 1 < argv.size())
                o_dir = parse_wpa_ctrl_value(argv[++i]);
            else if (a.rfind("-O", 0) == 0 && a.size() > 2)
                o_dir = parse_wpa_ctrl_value(a.substr(2));
            else if (a == "-C" && i + 1 < argv.size())
                c_val = argv[++i];
            else if (a.rfind("-C", 0) == 0 && a.size() > 2)
                c_val = a.substr(2);
            else if (a == "-c" && i + 1 < argv.size())
                conf_path = argv[++i];
            else if (a.rfind("-c", 0) == 0 && a.size() > 2)
                conf_path = a.substr(2);
        }

        if (!o_dir.empty())
            return o_dir;
        if (!conf_path.empty()) {
            std::string conf_dir = helix::wifi::detail::read_ctrl_interface_from_conf(conf_path);
            if (!conf_dir.empty())
                return conf_dir;
        }
        if (!c_val.empty())
            return parse_wpa_ctrl_value(std::move(c_val));
    }
    return {};
}

/// Ordered list of directories that may hold wpa_supplicant control sockets.
///
/// Standard distros use /run/wpa_supplicant (with the legacy /var/run alias).
/// Vendor firmwares sometimes relocate the control interface — Creality's K2
/// Plus launches `wpa_supplicant ... -O /etc/wifi/wpa_supplicant/sockets`, which
/// left network discovery broken until users hand-symlinked the sockets into
/// /var/run.  We therefore (1) honour an explicit override, (2) auto-detect the
/// live -O path from the running daemon, then (3) fall back to the known-good
/// locations (including the Creality path).
static std::vector<std::string> wpa_socket_dirs() {
    std::vector<std::string> dirs;
    auto add = [&dirs](std::string d) {
        // Trim trailing slashes so de-duplication is stable.
        while (d.size() > 1 && d.back() == '/')
            d.pop_back();
        if (!d.empty() && std::find(dirs.begin(), dirs.end(), d) == dirs.end())
            dirs.push_back(std::move(d));
    };

    if (const char* env = std::getenv("HELIX_WPA_SOCKET_DIR"))
        add(env);
    add(detect_wpa_ctrl_dir_from_proc());
    add("/run/wpa_supplicant");
    add("/var/run/wpa_supplicant");
    add("/etc/wifi/wpa_supplicant/sockets"); // Creality K2 Plus / Creality OS

    return dirs;
}

/// Return the wpa_supplicant control-socket directory actually in use — the
/// first entry of wpa_socket_dirs() that exists and contains at least one
/// non-p2p control socket. Mirrors init_wpa()'s own socket search exactly (same
/// order, same is_socket + "p2p" filter) so resolve_and_store_interface()
/// probes candidates from the same directory init_wpa() just connected into.
static std::string resolve_wpa_ctrl_directory() {
    for (const auto& base_path : wpa_socket_dirs()) {
        std::error_code ec;
        if (!fs::is_directory(base_path, ec) || ec)
            continue;

        for (const auto& entry : fs::directory_iterator(base_path, ec)) {
            if (ec)
                break;
            if (fs::is_socket(entry.path()) &&
                entry.path().string().find("p2p") == std::string::npos) {
                return base_path;
            }
        }
    }
    return {};
}

WifiBackendWpaSupplicant::WifiBackendWpaSupplicant()
    : hv::EventLoopThread(nullptr), conn(nullptr),
      mon_conn(nullptr) // Initialize monitor connection
{
    spdlog::debug("[WifiBackend] Initialized (wpa_supplicant mode)");
}

WifiBackendWpaSupplicant::~WifiBackendWpaSupplicant() {
    spdlog::trace("[WifiBackend] Destructor called");

    // Signal the init thread to abort (it checks this flag between operations)
    shutdown_requested_ = true;

    // Join the async init worker (if any) so it can't call start() into us
    // while we're tearing down.
    if (async_init_thread_.joinable()) {
        async_init_thread_.join();
    }

    // Stop the event loop and join the thread BEFORE freeing resources.
    // This prevents the use-after-free race (GitHub issue #8) where
    // cleanup_wpa() frees conn/mon_conn while init_wpa() is still using them.
    hv::EventLoopThread::stop();
    hv::EventLoopThread::join();

    // Thread is now fully stopped - safe to free resources
    cleanup_wpa();
}

WiFiError WifiBackendWpaSupplicant::start() {
    spdlog::debug("[WifiBackend] Starting wpa_supplicant backend...");
    shutdown_requested_ = false;

    // Pre-flight checks before starting event loop
    WiFiError preflight_result = check_system_prerequisites();
    if (!preflight_result.success()) {
        // User-facing critical error - wpa_supplicant not running or no permissions
        // In silent mode (e.g., HomePanel signal probe), only log - don't show modals
        if (is_silent()) {
            spdlog::debug("[WifiBackend] Pre-flight failed (silent mode): {}",
                          preflight_result.technical_msg);
        } else if (preflight_result.result == WiFiResult::SERVICE_NOT_RUNNING) {
            NOTIFY_ERROR_MODAL("WiFi Service Not Running",
                               "wpa_supplicant is not running. WiFi features unavailable.");
        } else if (preflight_result.result == WiFiResult::PERMISSION_DENIED) {
            NOTIFY_ERROR_MODAL("WiFi Permission Denied", "{}",
                               preflight_result.user_msg.empty() ? preflight_result.technical_msg
                                                                 : preflight_result.user_msg);
        } else {
            LOG_ERROR_INTERNAL("Pre-flight check failed: {}", preflight_result.technical_msg);
        }
        return preflight_result;
    }

    if (event_loop_active()) {
        // Thread already running AND a prior init actually connected — nothing to do.
        // We gate on init_succeeded_ (not init_complete_): a *failed* prior init
        // also sets init_complete_, and keying off it here would early-return
        // success and silently skip the retry, leaving the backend permanently
        // dead after a transient first-boot failure (helixscreen#1036).
        if (init_complete_.load() && init_succeeded_.load()) {
            spdlog::debug("[WifiBackend] Already running and initialized");
            return WiFiErrorHelper::success();
        }
        // Thread running but not connected (after stop(), or a prior init failed)
        // — re-initialize the wpa connections.
        spdlog::info("[WifiBackend] (Re)initializing WiFi on existing event loop");
        init_complete_ = false;
        loop()->runInLoop(std::bind(&WifiBackendWpaSupplicant::init_wpa, this));
    } else {
        // Start new event loop thread with initialization callback
        spdlog::info("[WifiBackend] Starting event loop thread");
        init_complete_ = false;
        try {
            hv::EventLoopThread::start(true, [this]() -> int {
                WifiBackendWpaSupplicant::init_wpa();
                return 0;
            });
        } catch (const std::exception& e) {
            return WiFiErrorHelper::connection_failed("Failed to start event loop: " +
                                                      std::string(e.what()));
        }
    }

    // Wait for init_wpa() to complete (with timeout)
    {
        std::unique_lock<std::mutex> lock(init_mutex_);
        if (!init_cv_.wait_for(lock, std::chrono::seconds(5),
                               [this] { return init_complete_.load(); })) {
            spdlog::error("[WifiBackend] Initialization timed out after 5 seconds");
            return WiFiError(WiFiResult::TIMEOUT, "Backend initialization timed out",
                             "WiFi system took too long to start");
        }
    }

    // init_complete_ fired, but that only means the attempt finished. If
    // init_wpa() bailed (socket not found, connect/attach failed) it dispatched
    // INIT_FAILED and left init_succeeded_ false. Report that honestly so
    // set_enabled() surfaces the failure and start_async() fires INIT_FAILED
    // (not READY) — and so the next start() retries instead of short-circuiting.
    if (!init_succeeded_.load()) {
        return WiFiErrorHelper::connection_failed(
            "wpa_supplicant init did not complete (no live control connection)");
    }

    spdlog::info("[WifiBackend] Backend initialized successfully");
    {
        const auto iface = resolved_interface();
        const std::string conf_path = (iface && !iface->conf_path.empty())
                                          ? iface->conf_path
                                          : detect_wpa_conf_path_from_proc();
        helix::wifi::remember_persistent_target(conf_path);
    }
    return WiFiErrorHelper::success();
}

void WifiBackendWpaSupplicant::start_async() {
    // Non-blocking variant: run start() on a worker thread. Fire READY on
    // success or INIT_FAILED on failure so callers can react without
    // blocking on socket discovery / event-loop startup.
    bool expected = false;
    if (!async_init_in_progress_.compare_exchange_strong(expected, true)) {
        spdlog::debug("[WifiBackend] wpa: start_async already in progress");
        return;
    }
    // Already connected — replay READY for late subscribers. A *failed* prior
    // init also sets init_complete_, so we additionally require init_succeeded_;
    // otherwise we'd falsely report READY and never retry (helixscreen#1036).
    if (init_complete_.load() && init_succeeded_.load()) {
        async_init_in_progress_ = false;
        dispatch_event("READY", "");
        return;
    }

    if (async_init_thread_.joinable()) {
        async_init_thread_.join();
    }

    // Wrap — EAGAIN under thread exhaustion throws std::system_error ([L083]).
    try {
        async_init_thread_ = std::thread([this]() {
            WiFiError result = start();
            bool ran_init = init_complete_.load();
            async_init_in_progress_ = false;
            if (result.success()) {
                dispatch_event("READY", "");
            } else if (!ran_init) {
                // start() failed before init_wpa() ran (preflight / thread spawn),
                // so no INIT_FAILED was dispatched there — emit it here. When
                // init_wpa() did run and fail it already dispatched INIT_FAILED;
                // re-dispatching would double-notify the manager.
                dispatch_event("INIT_FAILED", result.technical_msg);
            }
        });
    } catch (const std::system_error& e) {
        spdlog::error("[WifiBackend wpa] Failed to spawn init thread: {}", e.what());
        async_init_in_progress_ = false;
        dispatch_event("INIT_FAILED", "system busy");
    }
}

void WifiBackendWpaSupplicant::stop() {
    // Join any outstanding async init worker before teardown so it can't
    // race with cleanup_wpa() running on the event loop thread.
    if (async_init_thread_.joinable()) {
        async_init_thread_.join();
    }

    // NOTE: We intentionally do NOT stop the EventLoopThread here.
    // libhv's EventLoopThread doesn't support restart after stop(), so we keep
    // the thread running and just cleanup wpa connections. This allows set_enabled()
    // toggle to work reliably.

    if (!init_complete_.load()) {
        spdlog::trace("[WifiBackend] Already stopped (init not complete)");
        return;
    }

    spdlog::info("[WifiBackend] Disabling WiFi backend (keeping event loop alive)");

    // Reset init state so is_running() returns false and start() can re-init.
    // Clear init_succeeded_ too — the control connections are about to be torn
    // down, so the backend is no longer "up".
    init_succeeded_ = false;
    init_complete_ = false;

    // THREAD SAFETY: cleanup_wpa() manipulates libhv I/O handles (hio_read_stop,
    // hio_close) which MUST run on the event loop thread. Schedule via runInLoop()
    // with synchronization to ensure cleanup completes before stop() returns.
    //
    // The promise is held in a shared_ptr captured BY VALUE into the lambda. This
    // closes a use-after-free race (bundle WWZE4K9T, v0.99.96/pi32) where stop()'s
    // 2s wait_for times out — typically because the event loop is still inside a
    // prior init_wpa() blocked on wpa_ctrl_attach() for 5+ seconds — and the local
    // promise is destroyed before the queued cleanup lambda runs. The lambda would
    // then call set_value() on freed memory and crash in
    // std::__future_base::_State_baseV2::_M_do_set with PC=0. The shared_ptr keeps
    // the promise alive until the lambda releases its copy, regardless of whether
    // stop() has already returned.
    if (event_loop_active() && loop()) {
        auto cleanup_done = std::make_shared<std::promise<void>>();
        std::future<void> cleanup_future = cleanup_done->get_future();

        loop()->runInLoop([this, cleanup_done]() {
            cleanup_wpa();
            cleanup_done->set_value();
        });

        // Wait for cleanup to complete (with timeout to prevent deadlock). On
        // timeout the promise stays valid — the captured shared_ptr keeps it
        // alive until the deferred cleanup_wpa() eventually runs.
        if (cleanup_future.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
            spdlog::warn("[WifiBackend] Cleanup timed out after 2 seconds");
        }
    } else {
        // Event loop not running - cleanup directly (safe since no I/O callbacks can fire)
        cleanup_wpa();
    }

    spdlog::debug("[WifiBackend] WiFi backend disabled");
}

void WifiBackendWpaSupplicant::register_event_callback(
    const std::string& name, std::function<void(const std::string&)> callback) {
    // THREAD SAFETY: Lock callbacks map during access
    std::lock_guard<std::mutex> lock(callbacks_mutex_);

    const auto& entry = callbacks.find(name);
    if (entry == callbacks.end()) {
        callbacks.insert({name, callback});
        spdlog::debug("[WifiBackend] Registered callback '{}'", name);
    } else {
        // Callback already exists - could replace it, but parent doesn't
        LOG_WARN_INTERNAL("Callback '{}' already registered (not replacing)", name);
    }
}

// ============================================================================
// System Validation and Permission Checking
// ============================================================================

WiFiError WifiBackendWpaSupplicant::check_system_prerequisites() {
    spdlog::debug("[WifiBackend] Performing system prerequisites check");

    // 1. Check WiFi hardware availability
    WiFiError hw_result = check_wifi_hardware();
    if (!hw_result.success()) {
        return hw_result;
    }

    // 2. Check if any wpa_supplicant sockets exist
    std::vector<std::string> socket_paths = wpa_socket_dirs();
    bool socket_found = false;
    std::string accessible_socket;

    for (const auto& base_path : socket_paths) {
        if (fs::exists(base_path) && fs::is_directory(base_path)) {
            spdlog::debug("[WifiBackend] Found wpa_supplicant directory: {}", base_path);

            // Look for interface sockets (use error_code overload to handle permission denied)
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(base_path, ec)) {
                if (ec) {
                    spdlog::debug("[WifiBackend] Cannot iterate {}: {}", base_path, ec.message());
                    break;
                }
                if (fs::is_socket(entry.path())) {
                    std::string socket_path = entry.path().string();

                    // Skip P2P sockets
                    if (socket_path.find("p2p") == std::string::npos) {
                        socket_found = true;
                        spdlog::debug("[WifiBackend] Found wpa_supplicant socket: {}", socket_path);

                        // Check permissions for this socket
                        WiFiError perm_result = check_socket_permissions(socket_path);
                        if (perm_result.success()) {
                            accessible_socket = socket_path;
                            break;
                        } else {
                            LOG_WARN_INTERNAL("Socket {} permission check failed: {}", socket_path,
                                              perm_result.technical_msg);
                        }
                    }
                }
            }
            if (ec && !socket_found) {
                spdlog::debug("[WifiBackend] Permission denied iterating {}", base_path);
            }
            if (!accessible_socket.empty())
                break;
        }
    }

    if (!socket_found) {
        return WiFiErrorHelper::service_not_running("wpa_supplicant (no control sockets found)");
    }

    if (accessible_socket.empty()) {
        return WiFiErrorHelper::permission_denied("Found wpa_supplicant sockets but cannot access "
                                                  "them - check user permissions (netdev group)");
    }

    spdlog::debug("[WifiBackend] System prerequisites check passed - accessible socket: {}",
                  accessible_socket);
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendWpaSupplicant::check_socket_permissions(const std::string& socket_path) {
    spdlog::trace("[WifiBackend] Checking permissions for socket: {}", socket_path);

    // Try to open a test connection (use wpa_ctrl_open2 to avoid PrivateTmp issues)
    const char* cli_dir = resolve_wpa_client_dir();
    struct wpa_ctrl* test_ctrl = wpa_ctrl_open2(socket_path.c_str(), cli_dir);
    if (!test_ctrl) {
        // Get more specific error information
        int err = errno;
        std::string error_detail = "wpa_ctrl_open failed: " + std::string(strerror(err));

        if (err == EACCES || err == EPERM) {
            return WiFiErrorHelper::permission_denied(error_detail +
                                                      " (try adding user to netdev group)");
        } else if (err == ENOENT) {
            return WiFiErrorHelper::service_not_running("wpa_supplicant socket not found");
        } else if (err == ECONNREFUSED) {
            return WiFiErrorHelper::service_not_running("wpa_supplicant daemon not responding");
        } else {
            return WiFiErrorHelper::connection_failed(error_detail);
        }
    }

    // Test connection successful - close it immediately
    wpa_ctrl_close(test_ctrl);
    spdlog::debug("[WifiBackend] Socket permission check passed: {}", socket_path);
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendWpaSupplicant::check_wifi_hardware() {
    spdlog::trace("[WifiBackend] Checking WiFi hardware availability");

    // Check for common WiFi interface patterns in /sys/class/net
    //
    // Collect them all rather than breaking on the first hit. directory_iterator
    // order is filesystem order, so on a two-adapter device the old code logged
    // whichever name the kernel happened to hand back first — the AD5X bundles
    // report "interface present: wlan1" for a session that managed wlan0
    // throughout, which reads as a mismatch that is not one. The managed
    // interface is resolve_and_store_interface()'s call; this check only decides
    // whether any wireless hardware exists at all.
    std::vector<std::string> wifi_ifaces;

    try {
        const std::string net_path = "/sys/class/net";
        if (fs::exists(net_path)) {
            for (const auto& entry : fs::directory_iterator(net_path)) {
                std::string iface = entry.path().filename().string();

                // Check for common WiFi interface patterns
                if (iface.find("wlan") == 0 || iface.find("wlp") == 0 || iface.find("wlx") == 0 ||
                    iface.find("wifi") == 0) {
                    // Verify it's a wireless interface by checking for wireless directory
                    std::string wireless_path = entry.path().string() + "/wireless";
                    if (fs::exists(wireless_path))
                        wifi_ifaces.push_back(iface);
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        LOG_WARN_INTERNAL("Error checking WiFi interfaces: {}", e.what());
        // Don't fail entirely - this might be a permission issue or unusual system
    }

    if (wifi_ifaces.empty()) {
        return WiFiErrorHelper::hardware_not_available();
    }

    std::sort(wifi_ifaces.begin(), wifi_ifaces.end());
    const std::string interface_name = wifi_ifaces.front();
    // Manual join — fmt::join needs fmt/ranges.h, which spdlog's bundled copy
    // does not put on the include path here (see gcode_parser.cpp:777).
    std::string iface_list;
    for (const auto& n : wifi_ifaces)
        iface_list += (iface_list.empty() ? "" : ", ") + n;
    spdlog::debug("[WifiBackend] WiFi-capable interface(s) present: {}", iface_list);

    // Check RF-kill status
    try {
        const std::string rfkill_path = "/sys/class/rfkill";
        if (fs::exists(rfkill_path)) {
            for (const auto& entry : fs::directory_iterator(rfkill_path)) {
                std::string type_file = entry.path().string() + "/type";
                if (fs::exists(type_file)) {
                    std::ifstream type_stream(type_file);
                    std::string type;
                    if (type_stream >> type && type == "wlan") {
                        // A HARD block is a physical switch — we genuinely
                        // cannot clear it from software, so it stays fatal.
                        std::string hard_file = entry.path().string() + "/hard";
                        if (fs::exists(hard_file)) {
                            std::ifstream hard_stream(hard_file);
                            int hard_blocked;
                            if (hard_stream >> hard_blocked && hard_blocked == 1) {
                                return WiFiErrorHelper::rf_kill_blocked();
                            }
                        }

                        // A SOFT block is state OUR OWN application can set
                        // (set_radio_enabled(false)) — treating it as a fatal
                        // preflight failure means init_wpa() never runs, READY
                        // never fires, and set_radio_enabled(true) is never
                        // reachable to clear it: a permanent self-inflicted
                        // lockout that even survives a reboot, since
                        // systemd-rfkill.service persists and restores the
                        // soft block. Demote to a warning and let startup
                        // proceed so the normal unblock paths can run.
                        std::string soft_file = entry.path().string() + "/soft";
                        if (fs::exists(soft_file)) {
                            std::ifstream soft_stream(soft_file);
                            int soft_blocked;
                            if (soft_stream >> soft_blocked && soft_blocked == 1) {
                                spdlog::warn(
                                    "[WifiBackend] WiFi radio is soft rfkill-blocked ({}) — "
                                    "starting anyway so it can be cleared via set_radio_enabled",
                                    soft_file);
                            }
                        }
                        break;
                    }
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        LOG_WARN_INTERNAL("Error checking RF-kill status: {}", e.what());
        // Continue - RF-kill check is nice-to-have
    }

    spdlog::debug("[WifiBackend] WiFi hardware check passed - interface: {}", interface_name);
    return WiFiErrorHelper::success();
}

// ============================================================================
// wpa_supplicant Communication
// ============================================================================

void WifiBackendWpaSupplicant::init_wpa() {
    spdlog::trace("[WifiBackend] init_wpa() called in event loop thread");

    // Clear the success flag up front; only the fully-connected tail below sets
    // it true. Every early-return failure path therefore leaves it false.
    init_succeeded_ = false;

    // Socket discovery: Try common paths
    std::string wpa_socket;
    bool socket_found = false;

    // Try common wpa_supplicant socket paths (plus override / auto-detected / vendor)
    // Use error_code overload to handle permission denied gracefully (non-root users)
    std::vector<std::string> socket_dirs = wpa_socket_dirs();
    for (const auto& base_path : socket_dirs) {
        if (socket_found)
            break;
        if (!fs::exists(base_path) || !fs::is_directory(base_path))
            continue;

        spdlog::debug("[WifiBackend] Searching for wpa_supplicant socket in {}", base_path);

        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(base_path, ec)) {
            if (ec) {
                spdlog::debug("[WifiBackend] Cannot iterate {}: {}", base_path, ec.message());
                break;
            }
            if (fs::is_socket(entry.path())) {
                std::string socket_path = entry.path().string();

                // Filter out P2P sockets (e.g., p2p-dev-wlan0)
                if (socket_path.find("p2p") == std::string::npos) {
                    wpa_socket = socket_path;
                    socket_found = true;
                    spdlog::debug("[WifiBackend] Found wpa_supplicant socket: {}", wpa_socket);
                    break;
                }
            }
        }
    }

    // Helper to signal completion and notify waiters
    auto signal_init_complete = [this]() {
        init_complete_ = true;
        init_cv_.notify_all();
    };

    if (!socket_found) {
        LOG_ERROR_INTERNAL(
            "Could not find wpa_supplicant socket in any known location "
            "(/run, /var/run, /etc/wifi/wpa_supplicant/sockets, $HELIX_WPA_SOCKET_DIR)");
        LOG_ERROR_INTERNAL("Is wpa_supplicant daemon running? Override its control dir "
                           "with HELIX_WPA_SOCKET_DIR if it uses a non-standard path.");
        dispatch_event("INIT_FAILED", "wpa_supplicant socket not found");
        signal_init_complete();
        return;
    }

    // Check for shutdown before opening connections
    if (shutdown_requested_.load()) {
        spdlog::debug("[WifiBackend] Shutdown requested during init, aborting");
        signal_init_complete();
        return;
    }

    // Use wpa_ctrl_open2() so the client reply socket is created outside
    // systemd's PrivateTmp namespace (wpa_supplicant must be able to sendto()
    // the reply address).  Falls back to default /tmp for non-systemd runs.
    const char* cli_dir = resolve_wpa_client_dir();
    if (cli_dir) {
        spdlog::debug("[WifiBackend] Using client socket dir: {}", cli_dir);
    }

    // Open control connection (for sending commands)
    if (conn == nullptr) {
        conn = wpa_ctrl_open2(wpa_socket.c_str(), cli_dir);
        if (conn == nullptr) {
            LOG_ERROR_INTERNAL("Failed to open control connection to {}", wpa_socket);
            dispatch_event("INIT_FAILED", "Failed to connect to wpa_supplicant");
            signal_init_complete();
            return;
        }
        spdlog::debug("[WifiBackend] Opened control connection");
    }

    // Check for shutdown before opening monitor connection
    if (shutdown_requested_.load()) {
        spdlog::debug("[WifiBackend] Shutdown requested during init, aborting");
        signal_init_complete();
        return;
    }

    // Open monitor connection (for receiving events)
    mon_conn = wpa_ctrl_open2(wpa_socket.c_str(),
                              cli_dir); // SECURITY: Use member variable to prevent leak
    if (mon_conn == nullptr) {
        LOG_ERROR_INTERNAL("Failed to open monitor connection to {}", wpa_socket);
        dispatch_event("INIT_FAILED", "Failed to connect to wpa_supplicant monitor");
        signal_init_complete();
        return;
    }

    // Check for shutdown before the potentially blocking wpa_ctrl_attach().
    // This is the critical check: wpa_ctrl_attach() can block for 5+ seconds
    // when wpa_supplicant is unresponsive, and the destructor may be waiting.
    if (shutdown_requested_.load()) {
        spdlog::debug("[WifiBackend] Shutdown requested before attach, aborting");
        wpa_ctrl_close(mon_conn);
        mon_conn = nullptr;
        signal_init_complete();
        return;
    }

    // Attach to wpa_supplicant event stream
    if (wpa_ctrl_attach(mon_conn) != 0) {
        LOG_ERROR_INTERNAL("Failed to attach to wpa_supplicant events");
        dispatch_event("INIT_FAILED", "Failed to attach to wpa_supplicant events");
        wpa_ctrl_close(mon_conn);
        mon_conn = nullptr; // Clear member to avoid double-close
        signal_init_complete();
        return;
    }
    spdlog::debug("[WifiBackend] Attached to wpa_supplicant event stream");

    // Get file descriptor for monitor socket
    int monfd = wpa_ctrl_get_fd(mon_conn);
    if (monfd < 0) {
        LOG_ERROR_INTERNAL("Failed to get monitor socket file descriptor");
        dispatch_event("INIT_FAILED", "Failed to initialize wpa_supplicant communication");
        wpa_ctrl_close(mon_conn);
        mon_conn = nullptr; // Clear member to avoid double-close
        signal_init_complete();
        return;
    }
    spdlog::trace("[WifiBackend] Monitor socket fd: {}", monfd);

    // Register with libhv event loop for async I/O
    mon_io_ = hio_get(loop()->loop(), monfd);
    if (mon_io_ == nullptr) {
        LOG_ERROR_INTERNAL("Failed to register monitor socket with libhv");
        dispatch_event("INIT_FAILED", "Failed to initialize WiFi event handling");
        wpa_ctrl_close(mon_conn);
        mon_conn = nullptr;
        signal_init_complete();
        return;
    }

    // Set up I/O callbacks
    hio_set_context(mon_io_, this); // Store 'this' pointer for static callback
    hio_setcb_read(mon_io_, WifiBackendWpaSupplicant::_handle_wpa_events); // Static trampoline
    hio_read_start(mon_io_); // Start monitoring socket for events

    resolve_and_store_interface();
    resolve_5ghz_support();
    reconcile_saved_networks();

    // Reached only with live control + monitor connections registered — the
    // backend is genuinely up. This is the one place init_succeeded_ goes true.
    init_succeeded_ = true;

    spdlog::debug("[WifiBackend] wpa_supplicant backend initialized successfully");
    signal_init_complete();
}

// Why we can or cannot see the wpa config, in one log-safe line: ownership and
// mode versus our own ids, which is what separates "vendor path is root-only and
// we dropped privileges" from "the path does not resolve in our mount namespace"
// from "the file is genuinely absent". A SAVE_CONFIG that replies OK while our
// re-read finds nothing is indistinguishable between those three, and the
// distinction decides the fix. No SSIDs or PSKs — only metadata.
static std::string describe_conf_access(const std::string& path) {
    if (path.empty())
        return "path=<unresolved>";

    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return fmt::format("stat failed: {} (euid={})", std::strerror(errno), ::geteuid());
    }
    const bool can_read = ::access(path.c_str(), R_OK) == 0;
    const bool can_write = ::access(path.c_str(), W_OK) == 0;
    return fmt::format("size={} mode={:04o} uid={} gid={} euid={} egid={} r={} w={}",
                       static_cast<long long>(st.st_size), st.st_mode & 07777, st.st_uid, st.st_gid,
                       ::geteuid(), ::getegid(), can_read, can_write);
}

void WifiBackendWpaSupplicant::resolve_and_store_interface() {
    helix::wifi::Roots roots;
    roots.ctrl = resolve_wpa_ctrl_directory();

    if (roots.ctrl.empty()) {
        spdlog::warn(
            "[WifiBackend] Interface resolution: no wpa_supplicant control directory found");
        std::lock_guard<std::mutex> lock(iface_mutex_);
        iface_.reset();
        return;
    }

    // Each candidate gets its OWN short-lived wpa_ctrl handle, opened, probed
    // with STATUS and closed here. This deliberately does NOT touch `conn` —
    // conn stays attached to whichever socket init_wpa() already picked, and
    // resolution needs to probe the OTHER candidates too. Reusing conn would
    // make every candidate report conn's own status and silently defeat the
    // resolver.
    const char* cli_dir = resolve_wpa_client_dir();
    helix::wifi::StatusProbe probe = [cli_dir](const std::string& socket_path) -> std::string {
        struct wpa_ctrl* probe_conn = wpa_ctrl_open2(socket_path.c_str(), cli_dir);
        if (probe_conn == nullptr)
            return {};

        char resp[4096];
        size_t len = sizeof(resp) - 1;
        const std::string cmd = "STATUS";
        int result = wpa_ctrl_request(probe_conn, cmd.c_str(), cmd.length(), resp, &len, nullptr);
        wpa_ctrl_close(probe_conn);

        if (result != 0 || len >= sizeof(resp))
            return {};
        resp[len] = '\0';
        return std::string(resp, len);
    };

    auto resolved = helix::wifi::resolve_interface(roots, probe);

    {
        std::lock_guard<std::mutex> lock(iface_mutex_);
        iface_ = resolved;
    }

    // Seed radio_enabled_ from the actual hardware state. The atomic
    // defaults to true and, until now, nothing ever corrected it from a real
    // rfkill read — so a radio already soft-blocked when the process starts
    // (the block survives a reboot; see check_wifi_hardware()) showed the UI
    // toggle ON over a radio that cannot associate, the same class of state
    // lie this branch exists to eliminate. No rfkill node at all means there
    // is nothing to query; default to enabled rather than reporting a false
    // "off".
    {
        bool hw_radio_enabled = true;
        if (resolved && !resolved->rfkill_node.empty()) {
            std::ifstream soft_stream(resolved->rfkill_node + "/soft");
            int soft_blocked = 0;
            if (soft_stream >> soft_blocked) {
                hw_radio_enabled = (soft_blocked != 1);
            }
        }
        radio_enabled_ = hw_radio_enabled;
        spdlog::debug("[WifiBackend] radio_enabled_ seeded from hardware: {}", hw_radio_enabled);
    }

    // The reporter's AD5X is unreachable for the duration, and the bundles
    // carry no process list — so log enough for the NEXT bundle to decide
    // whether a second wpa_supplicant was the reason SAVE_CONFIG "failed".
    // Interface names and paths are not secrets; SSIDs and PSKs never appear.
    for (const auto& d : helix::wifi::detail::list_wpa_daemons(roots.proc)) {
        spdlog::info("[WifiBackend] wpa_supplicant pid={} -i='{}' -c='{}'", d.pid, d.iface,
                     d.conf_path);
    }
    if (resolved) {
        // describe_conf_access() supersedes the bare writable= flag: on the
        // reporter's AD5X that flag was false while SAVE_CONFIG replied OK, and
        // "not writable by us" and "wpa cannot write it either" are different
        // claims that the flag alone cannot separate.
        spdlog::info("[WifiBackend] Managing {} via {} (conf='{}' [{}], rfkill='{}')",
                     resolved->netdev, resolved->ctrl_socket, resolved->conf_path,
                     describe_conf_access(resolved->conf_path),
                     resolved->rfkill_node.empty() ? "none" : resolved->rfkill_node);
    } else {
        spdlog::warn("[WifiBackend] Interface resolution inconclusive — using legacy detection");
    }
}

std::optional<helix::wifi::WifiInterface> WifiBackendWpaSupplicant::resolved_interface() const {
    std::lock_guard<std::mutex> lock(iface_mutex_);
    return iface_;
}

void WifiBackendWpaSupplicant::cleanup_wpa() {
    spdlog::trace("[WifiBackend] Cleaning up wpa_supplicant connections");

    // Stop libhv I/O monitoring BEFORE closing the socket
    // This prevents callbacks on a closed fd and allows re-registration
    if (mon_io_) {
        spdlog::trace("[WifiBackend] Stopping libhv I/O monitoring");
        hio_read_stop(mon_io_);
        hio_close(mon_io_);
        mon_io_ = nullptr;
    }

    // Close monitor connection (detach from events)
    if (mon_conn) {
        spdlog::trace("[WifiBackend] Detaching from wpa_supplicant events");
        wpa_ctrl_detach(mon_conn); // Detach from event stream
        wpa_ctrl_close(mon_conn);  // Close monitor connection
        mon_conn = nullptr;
    }

    // Close control connection (under lock to avoid closing while send_command is using it)
    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        if (conn) {
            spdlog::trace("[WifiBackend] Closing wpa_supplicant control connection");
            wpa_ctrl_close(conn);
            conn = nullptr;
        }
    }

    spdlog::debug("[WifiBackend] wpa_supplicant connections cleaned up");
}

std::string WifiBackendWpaSupplicant::map_event_to_callback(const std::string& event) {
    // Map wpa_supplicant events to registered callback names
    // Only actionable events are mapped; informational events return empty string

    if (event.find("CTRL-EVENT-SCAN-RESULTS") != std::string::npos) {
        return "SCAN_COMPLETE";
    }
    if (event.find("CTRL-EVENT-CONNECTED") != std::string::npos) {
        return "CONNECTED";
    }
    if (event.find("CTRL-EVENT-DISCONNECTED") != std::string::npos) {
        return "DISCONNECTED";
    }
    // Auth failures can come in multiple forms
    if (event.find("CTRL-EVENT-SSID-TEMP-DISABLED") != std::string::npos &&
        event.find("WRONG_KEY") != std::string::npos) {
        return "AUTH_FAILED";
    }
    if (event.find("CTRL-EVENT-AUTH-REJECT") != std::string::npos) {
        return "AUTH_FAILED";
    }

    // Informational events - no callback needed
    // SCAN-STARTED, BSS-ADDED, BSS-REMOVED, REGDOM-CHANGE, SIGNAL-CHANGE, etc.
    return "";
}

void WifiBackendWpaSupplicant::handle_wpa_events(void* data, int len) {
    if (data == nullptr || len <= 0) {
        LOG_WARN_INTERNAL("Received empty event");
        return;
    }

    // Convert to string (may contain newlines)
    std::string event = std::string(static_cast<char*>(data), len);

    spdlog::trace("[WifiBackend] Event received: {}", event);

    // Determine which callback (if any) should receive this event
    std::string callback_name = map_event_to_callback(event);

    if (callback_name.empty()) {
        // Informational event - no callback needed
        spdlog::trace("[WifiBackend] Ignoring informational event (no matching callback)");
        return;
    }

    // THREAD SAFETY: Copy callback out under the mutex, then release BEFORE
    // invoking. Holding callbacks_mutex_ across the callback invites deadlock
    // if a handler acquires another backend lock or re-enters the backend.
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        auto it = callbacks.find(callback_name);
        if (it == callbacks.end()) {
            spdlog::trace("[WifiBackend] No callback registered for event type: {}", callback_name);
            return;
        }
        cb = it->second;
    }

    spdlog::debug("[WifiBackend] Dispatching {} event to callback", callback_name);
    try {
        cb(event);
    } catch (const std::exception& e) {
        LOG_ERROR_INTERNAL("Exception in callback '{}': {}", callback_name, e.what());
    } catch (...) {
        LOG_ERROR_INTERNAL("Unknown exception in callback '{}'", callback_name);
    }
}

void WifiBackendWpaSupplicant::_handle_wpa_events(hio_t* io, void* data, int readbyte) {
    // Static trampoline: Extract instance pointer and forward to member function
    WifiBackendWpaSupplicant* instance = static_cast<WifiBackendWpaSupplicant*>(hio_context(io));
    if (instance) {
        instance->handle_wpa_events(data, readbyte);
    } else {
        LOG_ERROR_INTERNAL("Static callback invoked with NULL context");
    }
}

void WifiBackendWpaSupplicant::dispatch_event(const std::string& event_name,
                                              const std::string& message) {
    // Dispatch to a specific registered callback (for synthetic events like INIT_FAILED).
    // Copy the callback out under the mutex, then release BEFORE invoking — same
    // deadlock-avoidance rationale as handle_wpa_events().
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        auto it = callbacks.find(event_name);
        if (it == callbacks.end()) {
            return;
        }
        cb = it->second;
    }

    spdlog::debug("[WifiBackend] Dispatching synthetic event '{}': {}", event_name, message);
    try {
        cb(message);
    } catch (const std::exception& e) {
        LOG_ERROR_INTERNAL("Exception in callback '{}': {}", event_name, e.what());
    } catch (...) {
        LOG_ERROR_INTERNAL("Unknown exception in callback '{}'", event_name);
    }
}

// Helper function to sanitize commands for logging (remove passwords)
static std::string sanitize_command_for_log(const std::string& cmd) {
    // Check if command contains password
    if (cmd.find(" psk ") != std::string::npos) {
        size_t psk_pos = cmd.find(" psk ");
        return cmd.substr(0, psk_pos + 5) + "\"[REDACTED]\"";
    }
    return cmd;
}

std::string WifiBackendWpaSupplicant::send_command(const std::string& cmd) {
    // Lock to prevent concurrent access from LVGL thread (get_status) and
    // event loop thread (get_scan_results) — wpa_ctrl is not thread-safe.
    std::lock_guard<std::mutex> lock(cmd_mutex_);

    if (conn == nullptr) {
        // Normal during the brief window between start_async() dispatch and
        // init_wpa() opening the control connection — UI callers (e.g. the
        // home-panel network widget) race the worker thread on boot. Keep at
        // debug so it's available when troubleshooting but doesn't surface as
        // a user-visible warning in the shipping build.
        spdlog::debug("[WifiBackend] send_command called but not connected to wpa_supplicant");
        return "";
    }

    char resp[4096];
    size_t len = sizeof(resp) - 1;

    // SECURITY: Don't log passwords
    std::string safe_cmd = sanitize_command_for_log(cmd);
    spdlog::trace("[WifiBackend] Sending command: {}", safe_cmd);

    int result = wpa_ctrl_request(conn, cmd.c_str(), cmd.length(), resp, &len, nullptr);
    if (result != 0) {
        LOG_ERROR_INTERNAL("Command failed: {} (error code: {})", safe_cmd, result);
        return "";
    }

    // SECURITY: Validate len before using as array index
    if (len >= sizeof(resp)) {
        LOG_ERROR_INTERNAL("Response too large: {} bytes", len);
        return "";
    }

    // Null-terminate response
    resp[len] = '\0';

    // SECURITY: Don't log password responses
    if (cmd.find(" psk ") == std::string::npos) {
        spdlog::trace("[WifiBackend] Command response ({} bytes): {}", len, std::string(resp, len));
    } else {
        spdlog::trace("[WifiBackend] Command response ({} bytes): [REDACTED]", len);
    }

    return std::string(resp, len);
}

// ============================================================================
// WifiBackend Interface Implementation
// ============================================================================

bool WifiBackendWpaSupplicant::is_running() const {
    // "Running" means we have live wpa_supplicant connections, not merely that
    // an init attempt finished. Keying off init_succeeded_ (not init_complete_)
    // means a failed init reports as not-running, so trigger_scan()/is_enabled()
    // surface "not ready" honestly instead of a contradictory "up but every
    // command fails" state (helixscreen#1036). The event-loop thread may still
    // be alive after stop(); that's fine — init_succeeded_ is false then too.
    return init_succeeded_.load();
}

WiFiError WifiBackendWpaSupplicant::trigger_scan() {
    if (!is_running()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    const std::string result = send_command("SCAN");
    switch (helix::wifi::detail::classify_scan_reply(result)) {
    case helix::wifi::detail::ScanTrigger::Started:
        spdlog::debug("[WifiBackend] Scan triggered successfully");
        return WiFiErrorHelper::success();
    case helix::wifi::detail::ScanTrigger::AlreadyBusy:
        spdlog::debug("[WifiBackend] SCAN already in progress (reply '{}') — results will arrive "
                      "on the in-flight scan",
                      result);
        return WiFiErrorHelper::success();
    case helix::wifi::detail::ScanTrigger::NoReply:
        return WiFiErrorHelper::connection_failed("No response from wpa_supplicant SCAN command");
    case helix::wifi::detail::ScanTrigger::Failed:
        break;
    }
    return WiFiError(WiFiResult::BACKEND_ERROR, "wpa_supplicant SCAN command failed: " + result,
                     "Failed to start network scan", "Check WiFi interface status");
}

// Count SSIDs that were observed on more than one band. Logged (as a count, never
// as names) so a band problem leaves a breadcrumb in the log.
static size_t count_multi_band(const std::vector<WiFiNetwork>& networks) {
    size_t n = 0;
    for (const auto& net : networks) {
        if (net.band_mask != 0 && (net.band_mask & (net.band_mask - 1)) != 0) {
            ++n;
        }
    }
    return n;
}

WiFiError WifiBackendWpaSupplicant::get_scan_results(std::vector<WiFiNetwork>& networks) {
    if (!is_running()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    std::string raw = send_command("SCAN_RESULTS");
    if (raw.empty()) {
        return WiFiErrorHelper::connection_failed(
            "No response from wpa_supplicant SCAN_RESULTS command");
    }

    if (raw.find("FAIL") != std::string::npos) {
        return WiFiError(WiFiResult::BACKEND_ERROR, "wpa_supplicant SCAN_RESULTS failed: " + raw,
                         "Failed to retrieve scan results");
    }

    try {
        std::vector<WiFiNetwork> parsed = parse_scan_results(raw);
        networks = wifi_merge_networks_by_ssid(parsed);
        if (networks.size() < parsed.size()) {
            spdlog::debug("[WifiBackend] Deduplicated {} networks to {} unique SSIDs",
                          parsed.size(), networks.size());
        }
        spdlog::debug("[WifiBackend] Retrieved {} unique networks ({} on multiple bands)",
                      networks.size(), count_multi_band(networks));
        return WiFiErrorHelper::success();
    } catch (const std::exception& e) {
        return WiFiError(WiFiResult::BACKEND_ERROR,
                         "Failed to parse scan results: " + std::string(e.what()),
                         "Error processing network scan data");
    }
}

WifiBackendWpaSupplicant::WpaConfSnapshot WifiBackendWpaSupplicant::read_wpa_conf_after_save() {
    const auto iface = resolved_interface();
    WpaConfSnapshot snap;
    snap.path =
        (iface && !iface->conf_path.empty()) ? iface->conf_path : detect_wpa_conf_path_from_proc();
    if (!snap.path.empty()) {
        std::ifstream conf(snap.path, std::ios::binary);
        if (conf.is_open()) {
            snap.contents.assign(std::istreambuf_iterator<char>(conf),
                                 std::istreambuf_iterator<char>());
            // bad() and not fail(): the stream reaches EOF normally here, so
            // failbit is expected. badbit is a real read error (EIO on a dying
            // eMMC), and a truncated read must not pass as authoritative.
            snap.readable = !conf.bad();
        }
    }
    return snap;
}

void WifiBackendWpaSupplicant::mirror_if_volatile(const std::string& conf_path) {
    // Verified present (or verified absent) is not the same as verified
    // durable: wpa_supplicant writes its config with a temp file + rename(),
    // and that rename replaces a persistence symlink with a regular file. On
    // such a platform the state we just confirmed on the volatile path is
    // sitting in RAM, not on the storage the printer actually boots from.
    if (helix::wifi::detail::is_volatile_path(conf_path) &&
        !helix::wifi::persistent_target().empty()) {
        helix::wifi::mirror_to_persistent(conf_path);
    }
}

void WifiBackendWpaSupplicant::reconcile_saved_networks() {
    const auto saved = helix::wifi::store::load();
    if (saved.empty())
        return;

    const std::string list_reply = send_command("LIST_NETWORKS");

    size_t restored = 0;
    size_t already_present = 0;
    size_t skipped = 0;

    for (const auto& net : saved) {
        if (!helix::wifi::detail::find_network_id(list_reply, net.ssid).empty()) {
            ++already_present;
            continue;
        }

        // Defence in depth: connect_network() already validated these
        // characters before anything was written to the store, but the store
        // is a plain file on disk and this loop is about to splice both
        // fields into wpa_supplicant's quoted command protocol.
        const std::string clean_ssid =
            helix::wifi::detail::validate_wpa_string(net.ssid, "stored SSID");
        if (clean_ssid.empty()) {
            spdlog::warn("[WifiBackend] Reconcile: skipping a stored network with an invalid SSID");
            ++skipped;
            continue;
        }
        // An open network's empty PSK is fine; a non-empty-but-invalid one is
        // not — same distinction connect_network() applies to the live
        // password argument. Validated via the non-logging predicate, never
        // validate_wpa_string(): that function logs the offending byte on
        // failure, and this value is a secret read back from disk, not a
        // fresh value the user just typed.
        std::string clean_psk;
        if (!net.psk.empty()) {
            if (!helix::wifi::detail::wpa_string_is_valid(net.psk)) {
                spdlog::warn("[WifiBackend] Reconcile: skipping '{}' — stored PSK failed "
                             "validation",
                             helix::redact::ssid(net.ssid));
                ++skipped;
                continue;
            }
            clean_psk = net.psk;
        }

        std::string add_result = send_command("ADD_NETWORK");
        if (add_result.empty() || add_result == "FAIL\n") {
            spdlog::warn("[WifiBackend] Reconcile: ADD_NETWORK failed for a stored network");
            ++skipped;
            continue;
        }
        std::string network_id = add_result;
        if (!network_id.empty() && network_id.back() == '\n')
            network_id.pop_back();
        if (network_id.empty() || !std::all_of(network_id.begin(), network_id.end(),
                                               [](unsigned char c) { return std::isdigit(c); })) {
            spdlog::warn("[WifiBackend] Reconcile: unexpected ADD_NETWORK reply for a stored "
                         "network");
            ++skipped;
            continue;
        }

        bool ok =
            send_command("SET_NETWORK " + network_id + " ssid \"" + clean_ssid + "\"") == "OK\n";
        if (ok) {
            ok = clean_psk.empty()
                     ? send_command("SET_NETWORK " + network_id + " key_mgmt NONE") == "OK\n"
                     : send_command("SET_NETWORK " + network_id + " psk \"" + clean_psk + "\"") ==
                           "OK\n";
        }
        if (ok)
            ok = send_command("ENABLE_NETWORK " + network_id) == "OK\n";

        if (!ok) {
            spdlog::warn("[WifiBackend] Reconcile: failed to restore a stored network — removing "
                         "the partial entry");
            send_command("REMOVE_NETWORK " + network_id);
            ++skipped;
            continue;
        }

        ++restored;
    }

    if (restored > 0) {
        send_command("SAVE_CONFIG");
        spdlog::info("[WifiBackend] Reconcile: restored {} network(s) from HelixScreen's store "
                     "into wpa_supplicant ({} already present, {} skipped, {} total stored)",
                     restored, already_present, skipped, saved.size());
    } else if (skipped > 0) {
        spdlog::warn("[WifiBackend] Reconcile: {} stored network(s) already present, {} could not "
                     "be restored ({} total stored)",
                     already_present, skipped, saved.size());
    } else {
        spdlog::debug("[WifiBackend] Reconcile: {} stored network(s), all already present in "
                      "wpa_supplicant",
                      saved.size());
    }
}

WiFiError WifiBackendWpaSupplicant::connect_network(const std::string& ssid,
                                                    const std::string& password) {
    if (!is_running()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    // SECURITY: Validate inputs to prevent command injection
    std::string clean_ssid = helix::wifi::detail::validate_wpa_string(ssid, "SSID");
    if (clean_ssid.empty()) {
        return WiFiError(WiFiResult::INVALID_PARAMETERS,
                         "SSID contains invalid characters (quotes, control chars, etc.)",
                         "Invalid network name", "Check that the network name is correct");
    }

    std::string clean_password = helix::wifi::detail::validate_wpa_string(password, "password");
    if (!password.empty() && clean_password.empty()) {
        return WiFiErrorHelper::authentication_failed(ssid +
                                                      " (password contains invalid characters)");
    }

    spdlog::info("[WifiBackend] Connecting to network '{}'", helix::redact::ssid(clean_ssid));

    // Step 1: Reuse an existing saved entry for this SSID when one exists,
    // otherwise add a new one. Every connect used to ADD_NETWORK
    // unconditionally — a real user's wpa_supplicant had reached network id 7
    // for a handful of networks, and duplicate all-enabled entries give
    // wpa_supplicant more candidates to roam between after a reboot (a
    // plausible contributor to an unwanted 5GHz reassociation after the user
    // had explicitly forgotten that network).
    //
    // reused_existing tracks whether network_id was newly ADD_NETWORK'd by
    // THIS call. The failure-cleanup REMOVE_NETWORK calls below must only
    // fire when it was — removing a reused id would delete a network the
    // user already had saved, which is worse than the duplicate-entry bug
    // this reuse is fixing.
    std::string network_id =
        helix::wifi::detail::find_network_id(send_command("LIST_NETWORKS"), clean_ssid);
    const bool reused_existing = !network_id.empty();
    if (reused_existing) {
        spdlog::debug("[WifiBackend] Reusing network id {} for '{}'", network_id,
                      helix::redact::ssid(clean_ssid));
    } else {
        std::string add_result = send_command("ADD_NETWORK");
        if (add_result.empty() || add_result == "FAIL\n") {
            NOTIFY_ERROR("Failed to save WiFi network");
            return WiFiErrorHelper::connection_failed("Failed to add network to wpa_supplicant");
        }

        // Parse network ID (should be a number)
        network_id = add_result;
        // Remove trailing newline
        if (!network_id.empty() && network_id.back() == '\n') {
            network_id.pop_back();
        }

        // SECURITY: Validate network ID is actually a number
        for (char c : network_id) {
            if (!std::isdigit(c)) {
                return WiFiError(WiFiResult::BACKEND_ERROR,
                                 "wpa_supplicant returned invalid network ID: " + network_id,
                                 "Internal WiFi error", "Try restarting WiFi services");
            }
        }

        spdlog::debug("[WifiBackend] Added network with ID: {}", network_id);
    }

    // Step 2: Set SSID
    std::string set_ssid_cmd = "SET_NETWORK " + network_id + " ssid \"" + clean_ssid + "\"";
    std::string ssid_result = send_command(set_ssid_cmd);
    if (ssid_result != "OK\n") {
        LOG_ERROR_INTERNAL("Failed to set SSID: {}", ssid_result);
        // Clean up: remove the network we just added. Never remove a reused
        // id — that would delete credentials the user already had saved.
        if (!reused_existing) {
            send_command("REMOVE_NETWORK " + network_id);
        }
        NOTIFY_ERROR("Failed to save WiFi network");
        return WiFiErrorHelper::connection_failed("Failed to configure network SSID");
    }

    // Step 3: Set security (PSK for secured networks, key_mgmt for open)
    if (clean_password.empty()) {
        // Open network
        std::string set_open_cmd = "SET_NETWORK " + network_id + " key_mgmt NONE";
        std::string open_result = send_command(set_open_cmd);
        if (open_result != "OK\n") {
            LOG_ERROR_INTERNAL("Failed to set open security: {}", open_result);
            // Never remove a reused id — see the Step 1 comment.
            if (!reused_existing) {
                send_command("REMOVE_NETWORK " + network_id);
            }
            NOTIFY_ERROR("Failed to save WiFi network");
            return WiFiErrorHelper::connection_failed("Failed to configure open network security");
        }
        spdlog::debug("[WifiBackend] Configured as open network");
    } else {
        // Secured network with PSK
        std::string set_psk_cmd = "SET_NETWORK " + network_id + " psk \"" + clean_password + "\"";
        std::string psk_result = send_command(set_psk_cmd);
        if (psk_result != "OK\n") {
            LOG_ERROR_INTERNAL(
                "Failed to set PSK"); // Don't log the actual result (may contain password)
            // Never remove a reused id — see the Step 1 comment.
            if (!reused_existing) {
                send_command("REMOVE_NETWORK " + network_id);
            }
            NOTIFY_ERROR("Failed to connect to '{}'. Check password.",
                         helix::redact::ssid(clean_ssid));
            return WiFiErrorHelper::authentication_failed(ssid);
        }
        spdlog::debug("[WifiBackend] Configured with PSK");
    }

    // Step 4: Enable and select network
    std::string enable_cmd = "ENABLE_NETWORK " + network_id;
    std::string enable_result = send_command(enable_cmd);
    if (enable_result != "OK\n") {
        LOG_ERROR_INTERNAL("Failed to enable network: {}", enable_result);
        // Never remove a reused id — see the Step 1 comment.
        if (!reused_existing) {
            send_command("REMOVE_NETWORK " + network_id);
        }
        NOTIFY_ERROR("Failed to save WiFi network");
        return WiFiErrorHelper::connection_failed("Failed to enable network configuration");
    }
    spdlog::debug("[WifiBackend] Network {} enabled, selecting for connection", network_id);

    // Step 5: Select network (disconnect others)
    std::string select_cmd = "SELECT_NETWORK " + network_id;
    std::string select_result = send_command(select_cmd);
    if (select_result != "OK\n") {
        LOG_ERROR_INTERNAL("Failed to select network: {}", select_result);
        // Never remove a reused id — see the Step 1 comment.
        if (!reused_existing) {
            send_command("REMOVE_NETWORK " + network_id);
        }
        NOTIFY_ERROR("Failed to connect to '{}'", helix::redact::ssid(clean_ssid));
        return WiFiErrorHelper::connection_failed("Failed to select network for connection");
    }

    // Step 6: Save config so WiFi persists across reboots
    // wpa_supplicant writes to its -c config file (which may be symlinked to
    // persistent storage by platform-specific scripts, e.g. Snapmaker U1's
    // /etc/network/if-pre-up.d/wpa-conf.sh)
    //
    // The reply is not trusted: the U1's wpa_supplicant answers OK and never
    // writes the file, so the old `save_result == "OK\n"` check logged
    // "saved to disk" while the credentials existed only in daemon memory and
    // died at the next power-off. Re-read the config and look for the SSID.
    const std::string save_result = send_command("SAVE_CONFIG");
    const auto conf = read_wpa_conf_after_save();

    if (helix::wifi::detail::classify_save_result(save_result, conf.contents, clean_ssid) ==
        helix::wifi::detail::SavePersistence::Persisted) {
        spdlog::debug("[WifiBackend] Credentials verified on disk at {}", conf.path);
        mirror_if_volatile(conf.path);
    } else if (conf.path.empty()) {
        spdlog::warn("[WifiBackend] Cannot verify credential persistence: no -c config path "
                     "found for the running wpa_supplicant (SAVE_CONFIG replied '{}')",
                     save_result);
        helix::wifi::store::save({clean_ssid, password});
    } else if (!conf.readable) {
        // Unreadable is not the same claim as "did not record it" — wpa may
        // have written the file perfectly well where we cannot look. Our store
        // is the fallback either way, so the outcome is unchanged; only the
        // message stops asserting something we did not observe.
        spdlog::warn("[WifiBackend] Cannot verify credential persistence: {} is not readable by "
                     "this process ({}) (SAVE_CONFIG replied '{}') — saved to HelixScreen's own "
                     "store instead",
                     conf.path, describe_conf_access(conf.path), save_result);
        helix::wifi::store::save({clean_ssid, password});
    } else {
        spdlog::warn("[WifiBackend] {} did not record this network (SAVE_CONFIG replied '{}') — "
                     "saved to HelixScreen's own store instead",
                     conf.path, save_result);
        helix::wifi::store::save({clean_ssid, password});
    }

    spdlog::info("[WifiBackend] Network configuration complete, connecting to '{}'",
                 helix::redact::ssid(ssid));
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendWpaSupplicant::disconnect_network() {
    if (!is_running()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    std::string result = send_command("DISCONNECT");
    if (result == "OK\n") {
        spdlog::debug("[WifiBackend] Disconnect successful");
        return WiFiErrorHelper::success();
    } else if (result.empty()) {
        return WiFiErrorHelper::connection_failed(
            "No response from wpa_supplicant DISCONNECT command");
    } else {
        return WiFiError(WiFiResult::BACKEND_ERROR, "wpa_supplicant DISCONNECT failed: " + result,
                         "Failed to disconnect from network");
    }
}

WiFiError WifiBackendWpaSupplicant::set_radio_enabled(bool on) {
    if (!is_running())
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");

    if (on) {
        // Unblock first: ENABLE_NETWORK on a soft-blocked radio associates with
        // nothing and the user sees a toggle that flips back. When a real
        // rfkill node exists but the unblock write fails, do NOT report
        // success — that would let the UI claim "on" over a radio that still
        // can't associate, exactly the bug this backend exists to fix. When
        // there is no rfkill node at all, this is an intentional quiet
        // degrade (association-only on/off) — leave that path alone.
        const auto iface = resolved_interface();
        const bool has_rfkill_node = iface && !iface->rfkill_node.empty();
        if (has_rfkill_node && !set_rfkill_soft_block(false)) {
            LOG_WARN_INTERNAL("Failed to clear rfkill soft-block on {}", iface->rfkill_node);
            return WiFiError(WiFiResult::RF_KILL_BLOCKED,
                             "Failed to clear rfkill soft-block on " + iface->rfkill_node,
                             "Could not turn WiFi radio on");
        }
        const std::string enabled = send_command("ENABLE_NETWORK all");
        if (enabled.compare(0, 2, "OK") != 0)
            LOG_WARN_INTERNAL("ENABLE_NETWORK all returned: {}", enabled);
        send_command("RECONNECT");
        spdlog::info("[WifiBackend] Radio enabled");
    } else {
        // Order matters: stop association while we can still talk to the
        // daemon, then block the radio. Reversed, the DISCONNECT may never
        // reach a blocked interface.
        send_command("DISCONNECT");
        const std::string disabled = send_command("DISABLE_NETWORK all");
        if (disabled.compare(0, 2, "OK") != 0)
            LOG_WARN_INTERNAL("DISABLE_NETWORK all returned: {}", disabled);
        if (!set_rfkill_soft_block(true)) {
            spdlog::info("[WifiBackend] No rfkill switch — radio off is association-only");
        }
        spdlog::info("[WifiBackend] Radio disabled");
    }

    radio_enabled_ = on;
    return WiFiErrorHelper::success();
}

bool WifiBackendWpaSupplicant::is_radio_enabled() const {
    return radio_enabled_.load();
}

WiFiError WifiBackendWpaSupplicant::forget_network(const std::string& ssid) {
    if (!is_running()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    std::string clean_ssid = helix::wifi::detail::validate_wpa_string(ssid, "SSID");
    if (clean_ssid.empty()) {
        return WiFiError(WiFiResult::INVALID_PARAMETERS,
                         "SSID contains invalid characters (quotes, control chars, etc.)",
                         "Invalid network name", "Check that the network name is correct");
    }

    const std::string network_id =
        helix::wifi::detail::find_network_id(send_command("LIST_NETWORKS"), clean_ssid);

    // A credential can live ONLY in HelixScreen's own store when SAVE_CONFIG
    // never reached the vendor's config (see wifi_saved_config.h) — check
    // both places before declaring "nothing to forget".
    const auto stored = helix::wifi::store::load();
    const bool had_store_entry = std::any_of(stored.begin(), stored.end(),
                                             [&](const auto& n) { return n.ssid == clean_ssid; });

    if (network_id.empty() && !had_store_entry) {
        spdlog::debug("[WifiBackend] forget_network: no saved entry for '{}'",
                      helix::redact::ssid(clean_ssid));
        return WiFiErrorHelper::network_not_found(ssid);
    }

    if (!network_id.empty()) {
        const std::string remove_result = send_command("REMOVE_NETWORK " + network_id);
        if (remove_result != "OK\n") {
            LOG_ERROR_INTERNAL("Failed to remove network {}: {}", network_id, remove_result);
            return WiFiError(WiFiResult::BACKEND_ERROR,
                             "wpa_supplicant REMOVE_NETWORK failed: " + remove_result,
                             "Failed to forget network");
        }

        // Same persistence problem connect_network() works around: SAVE_CONFIG's
        // reply is not proof of anything, and even a genuine write can land on
        // a volatile path whose persistence symlink a PRIOR SAVE_CONFIG already
        // replaced (see wifi_saved_config.h — device-verified on a Snapmaker
        // U1). Without re-checking and mirroring here, a forget on such a
        // device clears the daemon's live state and the volatile file, while
        // the persistent file the boot process actually reads still lists the
        // network — it comes back at the next power cycle, the exact
        // resurrection bug this whole feature exists to eliminate, reached by
        // a different route.
        //
        // Opposite polarity from connect_network()'s classify_save_result():
        // there, "OK" means the SSID should now be PRESENT on disk.
        // classify_save_result() is built around that direction (its
        // Persisted/NotPersisted pair both describe "is the SSID present"),
        // so it is the wrong shape to reuse here — a successful forget wants
        // the SSID ABSENT. wpa_config_has_network() is called directly
        // instead, on the same re-read config content read_wpa_conf_after_save()
        // fetches for connect_network() too.
        const std::string save_result = send_command("SAVE_CONFIG");
        const auto conf = read_wpa_conf_after_save();

        switch (helix::wifi::detail::classify_removal_result(!conf.path.empty(), conf.readable,
                                                             conf.contents, clean_ssid)) {
        case helix::wifi::detail::RemovalPersistence::Verified:
            spdlog::debug("[WifiBackend] Removal verified on disk at {}", conf.path);
            mirror_if_volatile(conf.path);
            break;

        case helix::wifi::detail::RemovalPersistence::StillListed:
            spdlog::warn("[WifiBackend] {} still lists '{}' after REMOVE_NETWORK + SAVE_CONFIG "
                         "(reply '{}') — the daemon dropped it from memory but the on-disk "
                         "config was not updated; it may reappear at the next boot",
                         conf.path, helix::redact::ssid(clean_ssid), save_result);
            break;

        case helix::wifi::detail::RemovalPersistence::Unverifiable:
            // Do NOT mirror, and do not claim a verified removal. An unreadable
            // config is empty here, which is indistinguishable from "the SSID is
            // gone" — and reading it as the latter is what let the forgotten
            // network come back at the next boot while the log said the removal
            // had been verified.
            if (conf.path.empty()) {
                spdlog::warn("[WifiBackend] Cannot verify removal persisted: no -c config path "
                             "found for the running wpa_supplicant (SAVE_CONFIG replied '{}')",
                             save_result);
            } else {
                spdlog::warn("[WifiBackend] Cannot verify removal persisted: {} is not readable "
                             "by this process ({}) (SAVE_CONFIG replied '{}') — if the vendor "
                             "config still lists it, the network returns at the next boot",
                             conf.path, describe_conf_access(conf.path), save_result);
            }
            break;
        }

        // Independent of the file: ask the daemon whether it still knows the
        // network. This works even where the config is unreadable, so it is the
        // one removal signal that is always available — and if REMOVE_NETWORK
        // replied OK while the entry survives, the config file was never the
        // problem in the first place.
        if (!helix::wifi::detail::find_network_id(send_command("LIST_NETWORKS"), clean_ssid)
                 .empty()) {
            spdlog::warn("[WifiBackend] wpa_supplicant still lists '{}' after REMOVE_NETWORK "
                         "replied OK — the daemon did not drop it",
                         helix::redact::ssid(clean_ssid));
        }
    }

    // Drop from HelixScreen's own store too, regardless of whether wpa_supplicant
    // had an entry — reconcile_saved_networks() re-adds anything left in the
    // store at the next backend init (every boot), which would silently undo
    // this forget the moment the printer power-cycles.
    helix::wifi::store::remove(clean_ssid);

    spdlog::info("[WifiBackend] Forgot network '{}'", helix::redact::ssid(clean_ssid));
    return WiFiErrorHelper::success();
}

bool WifiBackendWpaSupplicant::set_rfkill_soft_block(bool blocked) {
    const auto iface = resolved_interface();
    if (!iface || iface->rfkill_node.empty())
        return false;

    const std::string path = iface->rfkill_node + "/soft";
    std::ofstream f(path);
    if (!f.is_open()) {
        spdlog::debug("[WifiBackend] Cannot open {} — no rfkill control", path);
        return false;
    }
    f << (blocked ? "1" : "0");
    // operator<< only formats into the stream's userspace buffer — it does
    // not by itself trigger the write(2) into the sysfs node. Force that now
    // and check the state AFTER the flush, or a real write failure (denied
    // permission, driver quirk) reads as success because formatting always
    // succeeds.
    f.flush();
    if (!f.good()) {
        spdlog::debug("[WifiBackend] Write to {} failed", path);
        return false;
    }
    return true;
}

WifiBackend::ConnectionStatus WifiBackendWpaSupplicant::get_status() {
    ConnectionStatus status = {};
    status.connected = false;

    std::string raw_status = send_command("STATUS");
    if (raw_status.empty()) {
        // send_command already logs (at debug) when conn is null during the
        // startup race; post-init empty responses propagate upward as
        // status.connected=false which is the correct observable behavior.
        spdlog::debug("[WifiBackend] Empty STATUS response");
        return status;
    }

    // Parse key=value pairs from STATUS output
    std::istringstream stream(raw_status);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty())
            continue;

        // Find key=value separator
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos)
            continue;

        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        if (key == "wpa_state") {
            // Check if fully connected
            status.connected = (value == "COMPLETED");
        } else if (key == "ssid") {
            status.ssid = value;
        } else if (key == "bssid") {
            status.bssid = value;
        } else if (key == "address") {
            status.mac_address = value;
        } else if (key == "ip_address") {
            status.ip_address = value;
        } else if (key == "freq") {
            try {
                status.frequency_mhz = std::stoi(value);
            } catch (const std::exception&) {
                // Ignore parse errors
            }
        }
    }

    // If connected, get additional signal info via SIGNAL_POLL
    if (status.connected) {
        std::string signal_raw = send_command("SIGNAL_POLL");
        if (!signal_raw.empty()) {
            std::istringstream signal_stream(signal_raw);
            std::string signal_line;

            while (std::getline(signal_stream, signal_line)) {
                size_t eq_pos = signal_line.find('=');
                if (eq_pos == std::string::npos)
                    continue;

                std::string key = signal_line.substr(0, eq_pos);
                std::string value = signal_line.substr(eq_pos + 1);

                if (key == "RSSI") {
                    try {
                        int rssi_dbm = std::stoi(value);
                        status.signal_strength = dbm_to_percentage(rssi_dbm);
                    } catch (const std::exception& e) {
                        spdlog::trace("[WifiBackend] Invalid RSSI value '{}': {}", value, e.what());
                    }
                    break; // Found what we need
                }
            }
        }
    }

    // Only log when status actually changes (reduces log noise)
    bool status_changed =
        (status.connected != last_logged_status_.connected ||
         status.ssid != last_logged_status_.ssid ||
         status.ip_address != last_logged_status_.ip_address ||
         std::abs(status.signal_strength - last_logged_status_.signal_strength) > 5);

    if (status_changed) {
        spdlog::trace("[WifiBackend] Status: connected={} ssid='{}' ip='{}' signal={}%",
                      status.connected, status.ssid, status.ip_address, status.signal_strength);
        last_logged_status_ = status;
    }

    return status;
}

bool WifiBackendWpaSupplicant::supports_5ghz() const {
    return supports_5ghz_cached_;
}

void WifiBackendWpaSupplicant::resolve_5ghz_support() {
    try {
        if (supports_5ghz_resolved_)
            return;

        // Try wpa_supplicant GET_CAPABILITY freq
        std::string freq_resp = send_command("GET_CAPABILITY freq");
        if (wifi_parse_freq_list_has_5ghz(freq_resp)) {
            supports_5ghz_cached_ = true;
            supports_5ghz_resolved_ = true;
            spdlog::debug("[WifiBackend] 5GHz support detected via GET_CAPABILITY freq");
            return;
        }

        // Fallback: try iw phy info
        // Find phy name from /sys/class/net/wlan0/phy80211/name (or similar)
        FILE* pipe = popen("iw phy phy0 info 2>/dev/null", "r");
        if (pipe) {
            std::string iw_output;
            char buf[256];
            while (fgets(buf, sizeof(buf), pipe)) {
                iw_output += buf;
            }
            pclose(pipe);

            if (wifi_parse_iw_phy_has_5ghz(iw_output)) {
                supports_5ghz_cached_ = true;
                supports_5ghz_resolved_ = true;
                spdlog::debug("[WifiBackend] 5GHz support detected via iw phy info");
                return;
            }
        }

        supports_5ghz_resolved_ = true;
        spdlog::debug("[WifiBackend] No 5GHz support detected (2.4GHz only)");
    } catch (const std::exception& e) {
        spdlog::warn("[WifiBackend] Error detecting 5GHz support: {}", e.what());
        supports_5ghz_resolved_ = true;
        // Keep cached = false (safe default)
    }
}

// ============================================================================
// Helper Methods (encapsulate wpa_supplicant ugliness)
// ============================================================================

std::vector<WiFiNetwork> WifiBackendWpaSupplicant::parse_scan_results(const std::string& raw) {
    std::vector<WiFiNetwork> networks;

    if (raw.empty()) {
        spdlog::debug("[WifiBackend] Empty scan results");
        return networks;
    }

    // Skip header line (bssid / frequency / signal level / flags / ssid)
    std::istringstream stream(raw);
    std::string line;
    bool skip_header = true;

    while (std::getline(stream, line)) {
        if (skip_header) {
            skip_header = false;
            continue; // Skip "bssid / frequency / signal level / flags / ssid"
        }

        if (line.empty())
            continue;

        // Parse tab-separated fields: BSSID\tfreq\tsignal\tflags\tSSID
        // Note: Hidden networks may have only 4 fields (SSID completely absent)
        std::vector<std::string> fields = split_by_tabs(line);
        if (fields.size() < 4) {
            // Truly malformed - need at least BSSID, freq, signal, flags
            spdlog::trace("[WifiBackend] Skipping malformed scan line ({} fields): {}",
                          fields.size(), line);
            continue;
        }

        std::string bssid = fields[0];
        std::string freq_str = fields[1];
        std::string signal_str = fields[2];
        std::string flags = fields[3];
        // SSID may be missing entirely for hidden networks
        std::string ssid = (fields.size() >= 5) ? fields[4] : "";

        // Skip hidden networks (empty or missing SSID)
        if (ssid.empty()) {
            spdlog::trace("[WifiBackend] Skipping hidden network: {}", bssid);
            continue;
        }

        // Parse signal strength (dBm)
        int signal_dbm = 0;
        try {
            signal_dbm = std::stoi(signal_str);
        } catch (const std::exception& e) {
            LOG_WARN_INTERNAL("Invalid signal strength '{}': {}", signal_str, e.what());
            continue;
        }

        // Convert dBm to percentage
        int signal_percent = dbm_to_percentage(signal_dbm);

        // Detect security type
        bool is_secured = false;
        std::string security_type = detect_security_type(flags, is_secured);

        // Parse frequency
        int freq_mhz = 0;
        try {
            freq_mhz = std::stoi(freq_str);
        } catch (const std::exception&) {
            // Ignore parse errors, keep 0
        }

        // Create network entry
        WiFiNetwork network(ssid, signal_percent, is_secured, security_type, freq_mhz);
        networks.push_back(network);

        spdlog::trace("[WifiBackend] Parsed network: '{}' {}% {} {}", ssid, signal_percent,
                      security_type, bssid);
    }

    spdlog::debug("[WifiBackend] Parsed {} networks from scan results", networks.size());
    return networks;
}

std::vector<std::string> WifiBackendWpaSupplicant::split_by_tabs(const std::string& str) {
    std::vector<std::string> parts;
    std::stringstream ss(str);
    std::string part;
    while (std::getline(ss, part, '\t')) {
        parts.push_back(part);
    }
    return parts;
}

int WifiBackendWpaSupplicant::dbm_to_percentage(int dbm) {
    // -30 dBm = 100% (excellent), -90 dBm = 0% (unusable)
    return std::max(0, std::min(100, (dbm + 90) * 100 / 60));
}

std::string WifiBackendWpaSupplicant::detect_security_type(const std::string& flags,
                                                           bool& is_secured) {
    if (flags.find("WPA3") != std::string::npos) {
        is_secured = true;
        return "WPA3";
    }
    if (flags.find("WPA2") != std::string::npos) {
        is_secured = true;
        return "WPA2";
    }
    if (flags.find("WPA") != std::string::npos) {
        is_secured = true;
        return "WPA";
    }
    if (flags.find("WEP") != std::string::npos) {
        is_secured = true;
        return "WEP";
    }
    is_secured = false;
    return "Open";
}

#else
// ============================================================================
// macOS Stub Implementation: No-op for simulator
// ============================================================================

// Empty file - all methods are inline in header

#endif // !__APPLE__ && !__ANDROID__
