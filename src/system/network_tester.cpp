// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "network_tester.h"

#include "core/text_io.h"
#include "core/update_queue.h"
#include "helix_thread.h"
#include "spdlog/spdlog.h"

#ifdef __APPLE__
#include "core/run_capture.h"
#endif

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tio = helix::text_io;

// ============================================================================
// Constructor / Destructor
// ============================================================================

NetworkTester::NetworkTester() {
    spdlog::debug("[NetworkTester] Initialized");
}

NetworkTester::~NetworkTester() {
    // NOTE: Don't use spdlog here - during exit(), spdlog may already be destroyed
    // which causes a crash. Just silently clean up.

    // Signal cancellation to any running test
    cancelled_ = true;

    // MUST join thread if joinable, regardless of running_ state.
    // A completed test (running_=false) still has a joinable thread.
    // Destroying a joinable std::thread without join() calls std::terminate()!
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void NetworkTester::init_self_reference(std::shared_ptr<NetworkTester> self) {
    self_ = self;
    spdlog::debug("[NetworkTester] Self-reference initialized for async callback safety");
}

// ============================================================================
// Public API
// ============================================================================

void NetworkTester::start_test(Callback callback) {
    if (running_) {
        spdlog::warn("[NetworkTester] Test already running, ignoring start_test");
        return;
    }

    spdlog::info("[NetworkTester] Starting network connectivity test");

    // CRITICAL: Join any previous thread before starting new one.
    // If a previous test completed naturally, the thread is still joinable
    // even though running_ is false. Assigning to a joinable std::thread
    // causes std::terminate()!
    if (worker_thread_.joinable()) {
        spdlog::debug("[NetworkTester] Joining previous worker thread");
        worker_thread_.join();
    }

    callback_ = callback;
    running_ = true;
    cancelled_ = false;

    // Clear previous results
    result_ = TestResult{};

    // Spawn worker thread
    worker_thread_ = helix::make_thread(&NetworkTester::run_test, this);
}

void NetworkTester::cancel() {
    if (!running_) {
        return;
    }

    spdlog::debug("[NetworkTester] Cancelling test");
    cancelled_ = true;

    // Wait for thread to exit
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    running_ = false;
    spdlog::debug("[NetworkTester] Test cancelled");
}

bool NetworkTester::is_running() const {
    return running_;
}

// ============================================================================
// Worker Thread
// ============================================================================

void NetworkTester::run_test() {
    spdlog::debug("[NetworkTester] Worker thread started");

    // Step 1: Test gateway connectivity
    report_state(TestState::TESTING_GATEWAY);

    result_.gateway_ip = get_default_gateway();
    if (result_.gateway_ip.empty()) {
        result_.error_message = "No default gateway found";
        spdlog::warn("[NetworkTester] {}", result_.error_message);
        report_state(TestState::FAILED);
        running_ = false;
        return;
    }

    if (cancelled_) {
        spdlog::debug("[NetworkTester] Test cancelled during gateway lookup");
        running_ = false;
        return;
    }

    spdlog::debug("[NetworkTester] Testing gateway: {}", result_.gateway_ip);
    result_.gateway_ok = ping_host(result_.gateway_ip, 2);

    if (!result_.gateway_ok) {
        result_.error_message = "Gateway unreachable: " + result_.gateway_ip;
        spdlog::warn("[NetworkTester] {}", result_.error_message);
        report_state(TestState::FAILED);
        running_ = false;
        return;
    }

    if (cancelled_) {
        spdlog::debug("[NetworkTester] Test cancelled after gateway test");
        running_ = false;
        return;
    }

    // Step 2: Test internet connectivity
    report_state(TestState::TESTING_INTERNET);

    // Try Google DNS first, fallback to Cloudflare
    spdlog::debug("[NetworkTester] Testing internet: 8.8.8.8");
    result_.internet_ok = ping_host("8.8.8.8", 2);

    if (!result_.internet_ok && !cancelled_) {
        spdlog::debug("[NetworkTester] Testing internet: 1.1.1.1 (fallback)");
        result_.internet_ok = ping_host("1.1.1.1", 2);
    }

    if (cancelled_) {
        spdlog::debug("[NetworkTester] Test cancelled during internet test");
        running_ = false;
        return;
    }

    if (!result_.internet_ok) {
        result_.error_message = "Internet unreachable (gateway OK)";
        spdlog::warn("[NetworkTester] {}", result_.error_message);
    } else {
        spdlog::info("[NetworkTester] Network connectivity test passed");
    }

    // Report final state
    report_state(result_.internet_ok ? TestState::COMPLETED : TestState::FAILED);
    running_ = false;

    spdlog::debug("[NetworkTester] Worker thread finished");
}

void NetworkTester::report_state(TestState state) {
    if (!callback_) {
        spdlog::warn("[NetworkTester] No callback registered, ignoring state change");
        return;
    }

    // CRITICAL: This is called from worker thread - must dispatch to LVGL thread!
    spdlog::debug("[NetworkTester] Reporting state: {} (from worker thread)",
                  static_cast<int>(state));

    // Helper struct for async callback dispatch
    struct CallbackData {
        std::weak_ptr<NetworkTester> tester;
        TestState state;
        TestResult result;
    };

    // Use RAII-safe async callback wrapper
    helix::ui::queue_update<CallbackData>(
        std::make_unique<CallbackData>(CallbackData{self_, state, result_}),
        [](CallbackData* data) {
            spdlog::debug("[NetworkTester] Async callback executing in LVGL thread");

            // Safely check if tester still exists
            if (auto tester = data->tester.lock()) {
                if (tester->callback_) {
                    tester->callback_(data->state, data->result);
                } else {
                    spdlog::warn("[NetworkTester] Callback was cleared before async dispatch");
                }
            } else {
                spdlog::debug(
                    "[NetworkTester] Tester destroyed before async callback - safely ignored");
            }
        });
}

// ============================================================================
// Platform-Specific Helpers
// ============================================================================

std::string NetworkTester::get_default_gateway() {
#ifdef __APPLE__
    // macOS: Use 'route -n get default' and parse the 'gateway:' line
    int exit_code = -1;
    const std::string result = helix::run_capture("route -n get default 2>/dev/null", exit_code);
    if (exit_code != 0) {
        spdlog::warn("[NetworkTester] 'route' command failed with code {}", exit_code);
        return "";
    }

    // Parse output for "gateway: X.X.X.X"
    for (std::string_view line : tio::lines(result)) {
        if (line.find("gateway:") != std::string_view::npos) {
            // Extract IP after "gateway: "
            size_t pos = line.find(':');
            if (pos != std::string_view::npos) {
                std::string gateway(line.substr(pos + 1));
                // Trim whitespace
                gateway.erase(0, gateway.find_first_not_of(" \t\r\n"));
                gateway.erase(gateway.find_last_not_of(" \t\r\n") + 1);
                spdlog::debug("[NetworkTester] Found gateway: {}", gateway);
                return gateway;
            }
        }
    }

    spdlog::warn("[NetworkTester] No gateway found in route output");
    return "";

#else
    // Linux: Parse /proc/net/route for line with destination 00000000
    tio::LineReader route_file("/proc/net/route");
    if (!route_file) {
        spdlog::error("[NetworkTester] Failed to open /proc/net/route");
        return "";
    }

    std::string line;
    route_file.next(line); // Skip header

    while (route_file.next(line)) {
        auto tokens = tio::split_ws(line);
        if (tokens.size() < 3)
            continue;
        std::string destination(tokens[1]);
        std::string gateway(tokens[2]);

        // Default route has destination 00000000
        if (destination == "00000000") {
            // Gateway is in hex (little-endian), convert to IP
            if (gateway.length() == 8) {
                unsigned long gw_hex = std::strtoul(gateway.c_str(), nullptr, 16);
                char ip[16];
                snprintf(ip, sizeof(ip), "%lu.%lu.%lu.%lu", (gw_hex & 0xFF), (gw_hex >> 8) & 0xFF,
                         (gw_hex >> 16) & 0xFF, (gw_hex >> 24) & 0xFF);
                spdlog::debug("[NetworkTester] Found gateway: {}", ip);
                return ip;
            }
        }
    }

    spdlog::warn("[NetworkTester] No default gateway found in /proc/net/route");
    return "";
#endif
}

bool NetworkTester::ping_host(const std::string& host, int timeout_sec) {
#ifdef __APPLE__
    const char* timeout_flag = "-t";
#else
    const char* timeout_flag = "-W";
#endif

    std::string timeout_str = std::to_string(timeout_sec);

    spdlog::debug("[NetworkTester] Pinging {} (timeout={}s)", host, timeout_sec);

    // Use fork/execvp to avoid shell injection (host could be user-influenced)
    pid_t pid = fork();
    if (pid < 0) {
        spdlog::error("[NetworkTester] fork() failed: {}", strerror(errno));
        return false;
    }

    if (pid == 0) {
        // Child process — redirect stdout/stderr to /dev/null
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        // execvp does PATH lookup, no shell interpretation
        const char* argv[] = {"ping",       "-c",   "1", timeout_flag, timeout_str.c_str(),
                              host.c_str(), nullptr};
        execvp("ping", const_cast<char* const*>(argv));
        // If execvp returns, it failed
        _exit(127);
    }

    // Parent — wait for child
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        spdlog::error("[NetworkTester] waitpid() failed: {}", strerror(errno));
        return false;
    }

    bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    spdlog::debug("[NetworkTester] Ping {} {}", host, success ? "succeeded" : "failed");
    return success;
}
