// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_backend_networkmanager.h"

#include "spdlog/spdlog.h"

#if !defined(__APPLE__) && !defined(__ANDROID__)
// ============================================================================
// Linux Implementation: NetworkManager fallback via nmcli
// ============================================================================

#include "wifi_5ghz_detection.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

// ============================================================================
// Lifecycle
// ============================================================================

WifiBackendNetworkManager::WifiBackendNetworkManager() {
    spdlog::debug("[WifiBackend] Initialized (NetworkManager mode)");
}

WifiBackendNetworkManager::~WifiBackendNetworkManager() {
    spdlog::trace("[WifiBackend] NM destructor called");
    stop();
    // Use fprintf - spdlog may be destroyed during static cleanup
    fprintf(stderr, "[WifiBackend] NetworkManager backend destroyed\n");
}

WiFiError WifiBackendNetworkManager::start() {
    spdlog::debug("[WifiBackend] Starting NetworkManager backend...");

    // Serialize against concurrent start_async() workers. Hold the mutex
    // for the entire startup sequence — including the running_ check AND
    // status_thread_ assignment — so we can never double-spawn the status
    // thread (which would terminate() on the overwritten joinable thread).
    std::lock_guard<std::mutex> start_lock(start_mutex_);

    if (running_) {
        spdlog::debug("[WifiBackend] Already running");
        return WiFiErrorHelper::success();
    }

    // Check that NM is running and nmcli is available
    WiFiError prereq = check_system_prerequisites();
    if (!prereq.success()) {
        if (is_silent()) {
            spdlog::debug("[WifiBackend] NM pre-flight failed (silent mode): {}",
                          prereq.technical_msg);
        }
        return prereq;
    }

    // Detect WiFi interface
    wifi_interface_ = detect_wifi_interface();
    if (wifi_interface_.empty()) {
        WiFiError err = WiFiErrorHelper::hardware_not_available();
        if (is_silent()) {
            spdlog::debug("[WifiBackend] No WiFi interface found via NM (silent mode)");
        }
        return err;
    }

    spdlog::info("[WifiBackend] NetworkManager WiFi interface: {}", wifi_interface_);
    running_ = true;

    // Start background status polling thread. Wrap — EAGAIN throws ([L083]).
    status_running_ = true;
    try {
        status_thread_ = std::thread(&WifiBackendNetworkManager::status_thread_func, this);
    } catch (const std::system_error& e) {
        spdlog::error("[WifiBackend] Failed to spawn status thread: {}", e.what());
        status_running_ = false;
    }

    // Compute 5GHz support once (blocking here is fine — only happens at startup)
    if (!supports_5ghz_resolved_) {
        try {
            std::string props = exec_nmcli("-t -f WIFI-PROPERTIES device show " + wifi_interface_);
            supports_5ghz_cached_ = wifi_parse_nm_wifi_properties_has_5ghz(props);

            // Fallback: try iw if nmcli didn't give us a clear answer
            if (!supports_5ghz_cached_) {
                FILE* pipe = popen("iw phy phy0 info 2>/dev/null", "r");
                if (pipe) {
                    std::string iw_output;
                    char buf[256];
                    while (fgets(buf, sizeof(buf), pipe)) {
                        iw_output += buf;
                    }
                    pclose(pipe);
                    supports_5ghz_cached_ = wifi_parse_iw_phy_has_5ghz(iw_output);
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("[WifiBackend] NM: Error detecting 5GHz support: {}", e.what());
            // Keep cached = false (safe default)
        }
        supports_5ghz_resolved_ = true;
        spdlog::debug("[WifiBackend] NM: 5GHz support: {}", supports_5ghz_cached_.load());
    }

    return WiFiErrorHelper::success();
}

void WifiBackendNetworkManager::start_async() {
    // Non-blocking variant: run start() on a worker thread. Fire READY on
    // success or INIT_FAILED on failure so callers can react without
    // blocking the UI thread on subprocess probing.
    bool expected = false;
    if (!init_in_progress_.compare_exchange_strong(expected, true)) {
        spdlog::debug("[WifiBackend] NM: start_async already in progress");
        return;
    }
    if (running_) {
        init_in_progress_ = false;
        fire_event("READY");
        return;
    }

    // If a previous init thread ever ran, join it before replacing
    if (init_thread_.joinable()) {
        init_thread_.join();
    }

    // Wrap — EAGAIN under thread exhaustion throws std::system_error ([L083]).
    try {
        init_thread_ = std::thread([this]() {
            WiFiError result = start();
            // Fire the event BEFORE clearing init_in_progress_. A handler that
            // synchronously calls start_async() again (e.g. a re-entry via the
            // fallback path) must see init still "in progress" so the new call
            // is serialized against this still-running worker, instead of
            // short-circuiting and racing a joinable init_thread_.
            if (result.success()) {
                fire_event("READY");
            } else {
                fire_event("INIT_FAILED", result.technical_msg);
            }
            init_in_progress_ = false;
        });
    } catch (const std::system_error& e) {
        spdlog::error("[WifiBackend] Failed to spawn init thread: {}", e.what());
        init_in_progress_ = false;
        fire_event("INIT_FAILED", "system busy");
    }
}

void WifiBackendNetworkManager::stop() {
    // Ensure any in-flight async init completes before we tear down, so the
    // init thread can't call start() concurrently with stop().
    if (init_thread_.joinable()) {
        init_thread_.join();
    }

    if (!running_) {
        return;
    }

    spdlog::info("[WifiBackend] Stopping NetworkManager backend");

    // Signal threads to cancel
    scan_active_ = false;
    connect_active_ = false;

    // Stop status polling thread — hold CV mutex to ensure notify
    // is not lost if thread is between lock release and wait_for
    {
        std::lock_guard<std::mutex> lock(status_cv_mutex_);
        status_running_ = false;
    }
    status_cv_.notify_all();
    if (status_thread_.joinable()) {
        status_thread_.join();
    }

    // Join threads (MUST join, not detach - prevents use-after-free)
    if (scan_thread_.joinable()) {
        scan_thread_.join();
    }
    if (connect_thread_.joinable()) {
        connect_thread_.join();
    }

    running_ = false;
    spdlog::debug("[WifiBackend] NetworkManager backend stopped");
}

bool WifiBackendNetworkManager::is_running() const {
    return running_;
}

// ============================================================================
// Event System
// ============================================================================

void WifiBackendNetworkManager::register_event_callback(
    const std::string& name, std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);

    auto it = callbacks_.find(name);
    if (it == callbacks_.end()) {
        callbacks_.insert({name, callback});
        spdlog::debug("[WifiBackend] NM: Registered callback '{}'", name);
    } else {
        spdlog::warn("[WifiBackend] NM: Callback '{}' already registered (not replacing)", name);
    }
}

void WifiBackendNetworkManager::fire_event(const std::string& event_name, const std::string& data) {
    // Copy the callback out under the mutex, then release BEFORE invoking it.
    // Holding callbacks_mutex_ across the callback invites deadlock if a
    // handler acquires another backend lock (or re-enters the backend).
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        auto it = callbacks_.find(event_name);
        if (it == callbacks_.end()) {
            spdlog::trace("[WifiBackend] NM: No callback registered for '{}'", event_name);
            return;
        }
        cb = it->second;
    }

    spdlog::debug("[WifiBackend] NM: Firing event '{}'", event_name);
    try {
        cb(data);
    } catch (const std::exception& e) {
        spdlog::error("[WifiBackend] NM: Exception in callback '{}': {}", event_name, e.what());
    } catch (...) {
        spdlog::error("[WifiBackend] NM: Unknown exception in callback '{}'", event_name);
    }
}

// ============================================================================
// System Prerequisites
// ============================================================================

std::string WifiBackendNetworkManager::exec_nmcli(const std::string& args) {
    std::string cmd = "nmcli " + args + " 2>/dev/null";
    spdlog::trace("[WifiBackend] NM: exec: {}", cmd);

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        spdlog::debug("[WifiBackend] NM: popen failed for: {}", cmd);
        return "";
    }

    std::string result;
    std::array<char, 512> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

    int ret = pclose(pipe);
    int exit_code = WIFEXITED(ret) ? WEXITSTATUS(ret) : -1;

    if (exit_code != 0) {
        spdlog::trace("[WifiBackend] NM: nmcli exited with code {}", exit_code);
    }

    return result;
}

WifiBackendNetworkManager::NmcliResult
WifiBackendNetworkManager::exec_nmcli_full(const std::string& args) {
    // Capture stderr separately via a temp file, keeping stdout clean for parsing
    std::string stderr_file = "/tmp/nmcli_stderr_" + std::to_string(getpid());
    std::string cmd = "nmcli " + args + " 2>" + stderr_file;
    spdlog::trace("[WifiBackend] NM: exec_full: {}", cmd);

    NmcliResult nr;
    nr.exit_code = -1;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        spdlog::debug("[WifiBackend] NM: popen failed for: {}", cmd);
        unlink(stderr_file.c_str());
        return nr;
    }

    std::array<char, 512> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        nr.stdout_output += buffer.data();
    }

    int ret = pclose(pipe);
    nr.exit_code = WIFEXITED(ret) ? WEXITSTATUS(ret) : -1;

    // Read captured stderr
    FILE* err_fp = fopen(stderr_file.c_str(), "r");
    if (err_fp) {
        while (fgets(buffer.data(), buffer.size(), err_fp) != nullptr) {
            nr.stderr_output += buffer.data();
        }
        fclose(err_fp);
    }
    unlink(stderr_file.c_str());

    if (nr.exit_code != 0) {
        spdlog::trace("[WifiBackend] NM: nmcli exited with code {}, stderr: '{}'", nr.exit_code,
                      nr.stderr_output);
    }

    return nr;
}

bool WifiBackendNetworkManager::is_polkit_permission_error(const std::string& stderr_output) {
    if (stderr_output.empty()) {
        return false;
    }

    // Case-insensitive search for common polkit/permission denial indicators
    std::string lower = stderr_output;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    return lower.find("not authorized") != std::string::npos ||
           lower.find("permission denied") != std::string::npos ||
           lower.find("insufficient privilege") != std::string::npos ||
           lower.find("org.freedesktop.networkmanager") != std::string::npos ||
           lower.find("polkit") != std::string::npos;
}

WiFiError WifiBackendNetworkManager::check_system_prerequisites() {
    spdlog::debug("[WifiBackend] NM: Checking prerequisites");

    // Check if nmcli is available and NM is running, capturing stderr for diagnostics
    auto result = exec_nmcli_full("-t general status");

    if (result.stdout_output.empty() && result.exit_code != 0) {
        // Check stderr for polkit/permission errors before returning generic failure
        if (is_polkit_permission_error(result.stderr_output)) {
            spdlog::warn("[WifiBackend] NM: Polkit permission denied: {}", result.stderr_output);
            return WiFiError(WiFiResult::PERMISSION_DENIED,
                             "nmcli blocked by polkit: " + result.stderr_output,
                             "Permission denied - unable to manage WiFi",
                             "Re-run the HelixScreen installer, or see Troubleshooting docs");
        }
        return WiFiErrorHelper::service_not_running(
            "NetworkManager (nmcli not available or NM not running)");
    }

    // nmcli general status returns something like:
    // connected:full:enabled:enabled
    // Check that it's not "error" or empty
    if (result.stdout_output.find("error") != std::string::npos) {
        return WiFiErrorHelper::service_not_running(
            "NetworkManager (reported error: " + result.stdout_output + ")");
    }

    spdlog::debug("[WifiBackend] NM: Prerequisites check passed");
    return WiFiErrorHelper::success();
}

std::string WifiBackendNetworkManager::detect_wifi_interface() {
    // nmcli -t -f DEVICE,TYPE device status
    // Output: wlan0:wifi:connected:MyNetwork
    std::string output = exec_nmcli("-t -f DEVICE,TYPE device status");
    if (output.empty()) {
        return "";
    }

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        auto fields = split_nmcli_fields(line);
        if (fields.size() >= 2 && fields[1] == "wifi") {
            std::string iface = fields[0];
            // SECURITY: Validate interface name to prevent shell injection
            // Interface names should only contain alphanumeric chars, hyphens, underscores
            bool valid = !iface.empty();
            for (char c : iface) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
                    valid = false;
                    break;
                }
            }
            if (!valid) {
                spdlog::warn("[WifiBackend] NM: Suspicious interface name '{}', skipping", iface);
                continue;
            }
            spdlog::debug("[WifiBackend] NM: Detected WiFi interface: {}", iface);
            return iface;
        }
    }

    spdlog::debug("[WifiBackend] NM: No WiFi interface found in NM device list");
    return "";
}

// ============================================================================
// Scanning
// ============================================================================

WiFiError WifiBackendNetworkManager::trigger_scan() {
    if (!running_) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    // Clean up existing scan thread
    scan_active_ = false;
    if (scan_thread_.joinable()) {
        scan_thread_.join();
    }

    // Launch new scan thread. Wrap — EAGAIN throws ([L083]).
    scan_active_ = true;
    try {
        scan_thread_ = std::thread(&WifiBackendNetworkManager::scan_thread_func, this);
    } catch (const std::system_error& e) {
        spdlog::error("[WifiBackend] Failed to spawn scan thread: {}", e.what());
        scan_active_ = false;
        return WiFiError(WiFiResult::BACKEND_ERROR, "Could not start scan", "system busy");
    }
    return WiFiErrorHelper::success();
}

void WifiBackendNetworkManager::scan_thread_func() {
    spdlog::debug("[WifiBackend] NM: Scan thread started");

    // Request a rescan (may take a few seconds)
    exec_nmcli("device wifi rescan ifname " + wifi_interface_);

    // Wait for scan to complete (nmcli rescan is async)
    std::this_thread::sleep_for(std::chrono::seconds(2));

    if (!scan_active_) {
        spdlog::debug("[WifiBackend] NM: Scan cancelled");
        return;
    }

    // Get scan results
    std::string output =
        exec_nmcli("-t -f IN-USE,SSID,SIGNAL,SECURITY device wifi list ifname " + wifi_interface_);

    if (!scan_active_) {
        spdlog::debug("[WifiBackend] NM: Scan cancelled after fetch");
        return;
    }

    auto networks = parse_scan_output(output);

    // Cache results
    size_t found_count = 0;
    {
        std::lock_guard<std::mutex> lock(networks_mutex_);
        cached_networks_ = std::move(networks);
        found_count = cached_networks_.size();
    }

    spdlog::debug("[WifiBackend] NM: Scan complete, {} networks found", found_count);

    if (scan_active_) {
        fire_event("SCAN_COMPLETE");
    }
}

WiFiError WifiBackendNetworkManager::get_scan_results(std::vector<WiFiNetwork>& networks) {
    if (!running_) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    std::lock_guard<std::mutex> lock(networks_mutex_);
    networks = cached_networks_;
    return WiFiErrorHelper::success();
}

// ============================================================================
// Parsing
// ============================================================================

std::vector<std::string> WifiBackendNetworkManager::split_nmcli_fields(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;

    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            char next = line[i + 1];
            if (next == ':') {
                // Escaped colon - literal ':'
                current += ':';
                ++i;
            } else if (next == '\\') {
                // Escaped backslash - literal '\'
                current += '\\';
                ++i;
            } else {
                // Other escape - keep as-is
                current += line[i];
            }
        } else if (line[i] == ':') {
            // Field separator
            fields.push_back(current);
            current.clear();
        } else {
            current += line[i];
        }
    }

    // Don't forget the last field
    fields.push_back(current);
    return fields;
}

std::vector<WiFiNetwork> WifiBackendNetworkManager::parse_scan_output(const std::string& output) {
    std::vector<WiFiNetwork> networks;
    if (output.empty()) {
        return networks;
    }

    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }

        // nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY device wifi list
        // Format: IN-USE:SSID:SIGNAL:SECURITY
        // IN-USE is " " or "*"
        auto fields = split_nmcli_fields(line);
        if (fields.size() < 4) {
            spdlog::trace("[WifiBackend] NM: Skipping malformed scan line ({} fields): {}",
                          fields.size(), line);
            continue;
        }

        // fields[0] = IN-USE (* or space)
        std::string ssid = fields[1];
        std::string signal_str = fields[2];
        std::string security = fields[3];

        // Skip hidden networks (empty SSID)
        if (ssid.empty()) {
            continue;
        }

        // Parse signal strength (nmcli reports 0-100 percentage directly)
        int signal = 0;
        try {
            signal = std::stoi(signal_str);
        } catch (const std::exception&) {
            spdlog::trace("[WifiBackend] NM: Invalid signal '{}' for SSID '{}'", signal_str, ssid);
            continue;
        }

        // Clamp to valid range
        signal = std::max(0, std::min(100, signal));

        // Determine security type
        bool is_secured = false;
        std::string security_type = "Open";

        if (security.find("WPA3") != std::string::npos) {
            is_secured = true;
            security_type = "WPA3";
        } else if (security.find("WPA2") != std::string::npos) {
            is_secured = true;
            security_type = "WPA2";
        } else if (security.find("WPA") != std::string::npos) {
            is_secured = true;
            security_type = "WPA";
        } else if (security.find("WEP") != std::string::npos) {
            is_secured = true;
            security_type = "WEP";
        } else if (!security.empty() && security != "--") {
            // Unknown but non-empty security
            is_secured = true;
            security_type = security;
        }

        networks.emplace_back(ssid, signal, is_secured, security_type);
    }

    // Deduplicate by SSID (keep strongest signal) - same as wpa_supplicant backend
    if (networks.size() > 1) {
        std::unordered_map<std::string, size_t> best_by_ssid;
        for (size_t i = 0; i < networks.size(); ++i) {
            auto it = best_by_ssid.find(networks[i].ssid);
            if (it == best_by_ssid.end()) {
                best_by_ssid[networks[i].ssid] = i;
            } else if (networks[i].signal_strength > networks[it->second].signal_strength) {
                it->second = i;
            }
        }

        if (best_by_ssid.size() < networks.size()) {
            std::vector<WiFiNetwork> deduped;
            deduped.reserve(best_by_ssid.size());
            for (const auto& [ssid, idx] : best_by_ssid) {
                deduped.push_back(networks[idx]);
            }
            spdlog::debug("[WifiBackend] NM: Deduplicated {} networks to {} unique SSIDs",
                          networks.size(), deduped.size());
            networks = std::move(deduped);
        }
    }

    spdlog::debug("[WifiBackend] NM: Parsed {} networks from scan output", networks.size());
    return networks;
}

// ============================================================================
// Input Validation
// ============================================================================

std::string WifiBackendNetworkManager::validate_input(const std::string& input,
                                                      const std::string& field_name) {
    if (input.empty()) {
        spdlog::error("[WifiBackend] NM: Empty {}", field_name);
        return "";
    }

    if (input.length() > 255) {
        spdlog::error("[WifiBackend] NM: {} too long ({} chars)", field_name, input.length());
        return "";
    }

    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        // Reject control chars (0x00-0x1F) and DEL (0x7F)
        if (c < 32 || c == 127) {
            spdlog::error("[WifiBackend] NM: Invalid character in {}: ASCII {}", field_name,
                          static_cast<int>(c));
            return "";
        }
    }

    return input;
}

// ============================================================================
// Connection Management
// ============================================================================

WiFiError WifiBackendNetworkManager::connect_network(const std::string& ssid,
                                                     const std::string& password) {
    if (!running_) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    // Validate inputs
    std::string clean_ssid = validate_input(ssid, "SSID");
    if (clean_ssid.empty()) {
        return WiFiError(WiFiResult::INVALID_PARAMETERS,
                         "SSID contains invalid characters or is empty", "Invalid network name",
                         "Check that the network name is correct");
    }

    if (!password.empty()) {
        std::string clean_pass = validate_input(password, "password");
        if (clean_pass.empty()) {
            return WiFiErrorHelper::authentication_failed(
                ssid + " (password contains invalid characters)");
        }
    }

    spdlog::info("[WifiBackend] NM: Connecting to network '{}'", clean_ssid);

    // Clean up existing connect thread
    connect_active_ = false;
    if (connect_thread_.joinable()) {
        connect_thread_.join();
    }

    // Launch connection thread (uses fork/exec for security).
    // Pass SSID/password by value to avoid shared state race conditions.
    // Wrap — EAGAIN throws ([L083]).
    connect_active_ = true;
    try {
        connect_thread_ = std::thread(&WifiBackendNetworkManager::connect_thread_func, this,
                                      clean_ssid, password);
    } catch (const std::system_error& e) {
        spdlog::error("[WifiBackend] Failed to spawn connect thread: {}", e.what());
        connect_active_ = false;
        return WiFiError(WiFiResult::BACKEND_ERROR, "Could not start connect", "system busy");
    }

    return WiFiErrorHelper::success();
}

WifiBackendNetworkManager::ConnectAttempt
WifiBackendNetworkManager::try_nmcli_connect(const std::string& ssid, const std::string& password) {
    ConnectAttempt result;

    // SECURITY: fork/exec (no shell) so SSID/password can't be interpreted by sh.
    // Capture child's stderr via pipe so the caller can distinguish failure modes
    // (polkit denial, stale-profile key-mgmt error, etc.).

    int stderr_pipe[2];
    if (pipe(stderr_pipe) < 0) {
        spdlog::error("[WifiBackend] NM: pipe() failed: {}", strerror(errno));
        return result; // exit_code stays -1
    }

    pid_t pid = fork();
    if (pid < 0) {
        spdlog::error("[WifiBackend] NM: Fork failed: {}", strerror(errno));
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        return result;
    }

    if (pid == 0) {
        // Child: stderr → pipe, exec nmcli with no shell interpretation
        close(stderr_pipe[0]);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stderr_pipe[1]);

        if (password.empty()) {
            execlp("nmcli", "nmcli", "device", "wifi", "connect", ssid.c_str(), "ifname",
                   wifi_interface_.c_str(), nullptr);
        } else {
            execlp("nmcli", "nmcli", "device", "wifi", "connect", ssid.c_str(), "password",
                   password.c_str(), "ifname", wifi_interface_.c_str(), nullptr);
        }
        _exit(127); // exec failed
    }

    close(stderr_pipe[1]);

    constexpr int CONNECT_TIMEOUT_SECONDS = 30;
    constexpr int POLL_INTERVAL_MS = 100;
    auto start_time = std::chrono::steady_clock::now();

    int status = 0;
    bool child_done = false;

    while (!child_done) {
        if (!connect_active_) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            close(stderr_pipe[0]);
            spdlog::debug("[WifiBackend] NM: Connect cancelled");
            return result; // caller will see !connect_active_ and bail
        }

        pid_t wait_result = waitpid(pid, &status, WNOHANG);
        if (wait_result == pid) {
            child_done = true;
        } else if (wait_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            spdlog::error("[WifiBackend] NM: waitpid error: {}", strerror(errno));
            close(stderr_pipe[0]);
            return result;
        } else {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed > std::chrono::seconds(CONNECT_TIMEOUT_SECONDS)) {
                result.timed_out = true;
                kill(pid, SIGTERM);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                child_done = true;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
            }
        }
    }

    char buf[512];
    ssize_t n;
    while ((n = read(stderr_pipe[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        result.stderr_out += buf;
    }
    close(stderr_pipe[0]);

    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

bool WifiBackendNetworkManager::delete_connection_profile(const std::string& profile_id) {
    // SECURITY: fork/exec so profile_id (SSID-derived) can't be shell-interpreted.
    pid_t pid = fork();
    if (pid < 0) {
        spdlog::warn("[WifiBackend] NM: delete profile fork failed: {}", strerror(errno));
        return false;
    }
    if (pid == 0) {
        // Suppress stdout/stderr — we only care about exit code
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("nmcli", "nmcli", "connection", "delete", "id", profile_id.c_str(), nullptr);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return false;
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return code == 0;
}

void WifiBackendNetworkManager::connect_thread_func(std::string ssid, std::string password) {
    spdlog::debug("[WifiBackend] NM: Connect thread started for '{}'", ssid);

    ConnectAttempt attempt = try_nmcli_connect(ssid, password);

    if (!connect_active_) {
        return;
    }

    // nmcli quirk: `device wifi connect <ssid> password <psk>` reuses any
    // existing saved profile with the same SSID. If that profile is malformed
    // (e.g. created earlier without security), the password write lands but
    // key-mgmt stays empty and NM refuses with:
    //   Error: 802-11-wireless-security.key-mgmt: property is missing.
    // Self-heal once: delete the stale profile and retry.
    auto looks_like_stale_profile = [](const std::string& err) {
        return err.find("key-mgmt") != std::string::npos &&
               err.find("property is missing") != std::string::npos;
    };

    if (attempt.exit_code != 0 && !attempt.timed_out &&
        looks_like_stale_profile(attempt.stderr_out)) {
        spdlog::warn("[WifiBackend] NM: '{}' has a stale/malformed saved profile "
                     "(key-mgmt missing); deleting and retrying",
                     ssid);
        if (delete_connection_profile(ssid)) {
            spdlog::info("[WifiBackend] NM: Deleted stale profile for '{}', retrying connect",
                         ssid);
            attempt = try_nmcli_connect(ssid, password);
            if (!connect_active_) {
                return;
            }
        } else {
            spdlog::warn("[WifiBackend] NM: Could not delete stale profile for '{}' "
                         "(nmcli connection delete failed)",
                         ssid);
        }
    }

    if (attempt.timed_out) {
        spdlog::warn("[WifiBackend] NM: Connection to '{}' timed out", ssid);
        fire_event("DISCONNECTED", "Connection timed out");
        request_status_refresh();
        return;
    }

    if (attempt.exit_code == 0) {
        spdlog::info("[WifiBackend] NM: Connected to '{}'", ssid);
        fire_event("CONNECTED");
        request_status_refresh();
        return;
    }

    // Failure path
    if (is_polkit_permission_error(attempt.stderr_out)) {
        spdlog::warn("[WifiBackend] NM: Permission denied connecting to '{}': {}", ssid,
                     attempt.stderr_out);
        fire_event("AUTH_FAILED",
                   "Permission denied - check WiFi permissions. Re-run the HelixScreen "
                   "installer, or see Troubleshooting docs");
        request_status_refresh();
        return;
    }

    spdlog::warn("[WifiBackend] NM: Connection to '{}' failed (exit code {}{})", ssid,
                 attempt.exit_code,
                 attempt.stderr_out.empty() ? "" : ", stderr: " + attempt.stderr_out);
    // nmcli exit code 10 = connection timeout; doesn't cleanly distinguish auth
    // failure. Fire AUTH_FAILED as the best guess for secured networks.
    if (!password.empty()) {
        fire_event("AUTH_FAILED", "Connection failed");
    } else {
        fire_event("DISCONNECTED", "Connection failed");
    }
    request_status_refresh();
}

WiFiError WifiBackendNetworkManager::disconnect_network() {
    if (!running_) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    spdlog::info("[WifiBackend] NM: Disconnecting from WiFi");

    // nmcli device disconnect <iface>
    std::string result = exec_nmcli("device disconnect " + wifi_interface_);

    // nmcli reports success even if already disconnected
    spdlog::debug("[WifiBackend] NM: Disconnect result: {}", result);
    fire_event("DISCONNECTED");
    request_status_refresh();
    return WiFiErrorHelper::success();
}

// ============================================================================
// Status
// ============================================================================

WifiBackend::ConnectionStatus WifiBackendNetworkManager::get_status() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return cached_status_;
}

WifiBackend::ConnectionStatus WifiBackendNetworkManager::poll_status_now() {
    ConnectionStatus status = {};
    status.connected = false;
    status.signal_strength = 0;

    // Query GENERAL fields from device show for state, MAC, connection profile.
    // Note: WIFI.SSID is NOT a valid field for "device show" — it causes the
    // entire query to fail. The actual SSID must come from "device wifi list".
    std::string dev_info = exec_nmcli("-t -f GENERAL device show " + wifi_interface_);
    if (dev_info.empty()) {
        return status;
    }

    // Parse key:value pairs from device show output
    std::istringstream stream(dev_info);
    std::string line;

    while (std::getline(stream, line)) {
        auto fields = split_nmcli_fields(line);
        if (fields.size() < 2) {
            continue;
        }

        if (fields[0] == "GENERAL.STATE") {
            // State like "100 (connected)" or "30 (disconnected)"
            status.connected = (fields[1].find("connected") != std::string::npos) &&
                               (fields[1].find("disconnected") == std::string::npos);
        } else if (fields[0] == "GENERAL.HWADDR") {
            // nmcli doesn't escape colons in MAC addresses even in -t mode,
            // so split_nmcli_fields splits "2C:CF:67:2B:3C:01" into multiple fields.
            // Rejoin all fields after the key with ':' to reconstruct the MAC.
            std::string mac;
            for (size_t i = 1; i < fields.size(); ++i) {
                if (!mac.empty())
                    mac += ':';
                mac += fields[i];
            }
            status.mac_address = mac;
        } else if (fields[0] == "GENERAL.CONNECTION") {
            // NM profile name — used as fallback SSID if wifi list lookup fails
            if (fields[1] != "--") {
                status.ssid = fields[1];
            }
        }
    }

    // If connected, get SSID + signal from wifi list, and IP from device show.
    // GENERAL.CONNECTION gives the NM profile name which can be "preconfigured"
    // or other non-SSID values. The wifi list gives the real broadcast SSID.
    if (status.connected) {
        // Get actual SSID and signal from the IN-USE network in wifi list
        std::string wifi_info =
            exec_nmcli("-t -f IN-USE,SSID,SIGNAL device wifi list ifname " + wifi_interface_);
        if (!wifi_info.empty()) {
            std::istringstream wifi_stream(wifi_info);
            std::string wifi_line;
            while (std::getline(wifi_stream, wifi_line)) {
                auto wifi_fields = split_nmcli_fields(wifi_line);
                if (wifi_fields.size() >= 3 && wifi_fields[0] == "*") {
                    if (!wifi_fields[1].empty()) {
                        status.ssid = wifi_fields[1];
                    }
                    try {
                        status.signal_strength =
                            std::max(0, std::min(100, std::stoi(wifi_fields[2])));
                    } catch (...) {
                    }
                    break;
                }
            }
        }

        // Get IP address from device show (uses validated wifi_interface_)
        std::string ip_info = exec_nmcli("-t -f IP4.ADDRESS device show " + wifi_interface_);
        if (!ip_info.empty()) {
            std::istringstream ip_stream(ip_info);
            std::string ip_line;
            while (std::getline(ip_stream, ip_line)) {
                auto ip_fields = split_nmcli_fields(ip_line);
                if (ip_fields.size() >= 2 &&
                    ip_fields[0].find("IP4.ADDRESS") != std::string::npos) {
                    // Value is like "192.168.1.100/24"
                    std::string ip = ip_fields[1];
                    size_t slash = ip.find('/');
                    if (slash != std::string::npos) {
                        ip = ip.substr(0, slash);
                    }
                    status.ip_address = ip;
                    break;
                }
            }
        }
    }

    spdlog::trace("[WifiBackend] NM: Status: connected={} ssid='{}' ip='{}' signal={}%",
                  status.connected, status.ssid, status.ip_address, status.signal_strength);

    return status;
}

bool WifiBackendNetworkManager::supports_5ghz() const {
    return supports_5ghz_cached_;
}

// ============================================================================
// Background Status Polling
// ============================================================================

void WifiBackendNetworkManager::status_thread_func() {
    spdlog::debug("[WifiBackend] NM: Status polling thread started");

    constexpr auto POLL_INTERVAL = std::chrono::seconds(5);

    while (status_running_) {
        // Poll nmcli for current status
        ConnectionStatus fresh_status = {};
        if (running_) {
            fresh_status = poll_status_now();
        }

        // Update cache and fire events on connection state transitions
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            cached_status_ = fresh_status;
        }

        bool now_connected = fresh_status.connected;
        bool was_connected = prev_connected_.exchange(now_connected);

        if (now_connected && !was_connected) {
            spdlog::info("[WifiBackend] NM: Connection detected via status poll");
            fire_event("CONNECTED");
        } else if (!now_connected && was_connected) {
            spdlog::info("[WifiBackend] NM: Disconnection detected via status poll");
            fire_event("DISCONNECTED");
        }

        spdlog::trace(
            "[WifiBackend] NM: Status cache updated (connected={}, ssid='{}', signal={}%)",
            fresh_status.connected, fresh_status.ssid, fresh_status.signal_strength);

        // Sleep until next poll or wakeup signal.
        // Uses dedicated status_cv_mutex_ (not status_mutex_) so get_status() callers
        // aren't blocked during the 5-second wait. Safe because the predicates
        // (status_running_, status_refresh_requested_) are both atomic.
        {
            std::unique_lock<std::mutex> lock(status_cv_mutex_);
            status_cv_.wait_for(lock, POLL_INTERVAL, [this] {
                return !status_running_.load() || status_refresh_requested_.load();
            });
            status_refresh_requested_ = false;
        }
    }

    spdlog::debug("[WifiBackend] NM: Status polling thread exiting");
}

void WifiBackendNetworkManager::request_status_refresh() {
    // Set flag so the thread re-polls immediately even if notification
    // arrives while poll_status_now() is running (not waiting on CV).
    status_refresh_requested_ = true;
    // Lock CV mutex to ensure notify isn't lost between predicate
    // check and wait_for entry in status_thread_func
    { std::lock_guard<std::mutex> lock(status_cv_mutex_); }
    status_cv_.notify_one();
}

#endif // !__APPLE__ && !__ANDROID__
