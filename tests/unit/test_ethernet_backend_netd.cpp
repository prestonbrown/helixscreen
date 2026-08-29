// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * EthernetBackendNetd unit tests — the backend driven directly (not through
 * EthernetBackend::create(), which a later phase wires) against a fake netd
 * daemon (netd_test_server.h) repointed via HELIX_NETD_SOCKET, plus a fake
 * sysfs tree injected through the constructor's root argument.
 *
 * get_info() is a synchronous one-shot query, so every case that needs a
 * daemon reply runs it on a worker thread and pushes the reply from this one
 * while the query blocks in its read. Assertions are exact values (ip string,
 * interface name, mac bytes, status text) wherever the feature produces a
 * value, so a mutation anywhere in the mapping shows up red here.
 */

#include "../../include/ethernet_backend_netd.h"
#include "netd_test_server.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

#if !defined(__ANDROID__)

namespace {

namespace fs = std::filesystem;

/// Bounded polling wait (real clock, small step) — same shape as the wifi
/// netd tests: the fake server exposes no condition variable.
bool wait_until(const std::function<bool()>& pred, int timeout_ms = 5000, int step_ms = 10) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
    return pred();
}

} // namespace

// ============================================================================
// Fixture: fake sysfs tree (eth0 with an address file, plus lo/wlan0 the
// scan must skip) + fake daemon + repointed env.
// ============================================================================
class EthernetNetdFixture {
  public:
    EthernetNetdFixture() {
        char dir_template[] = "/tmp/helix_netd_eth_XXXXXX";
        char* dir = ::mkdtemp(dir_template);
        REQUIRE(dir != nullptr);
        dir_ = dir;

        const std::string net = dir_ + "/sys/class/net";
        fs::create_directories(net + "/eth0");
        fs::create_directories(net + "/lo");
        fs::create_directories(net + "/wlan0");
        // Trailing newline: the reader must trim.
        {
            std::ofstream address(net + "/eth0/address");
            address << "aa:bb:cc:12:34:56\n";
        }

        sock_env_.set(dir_ + "/netd.sock");
        bin_env_.unset();

        server_ = std::make_unique<helix_test::NetdFakeServer>();
        REQUIRE(server_->start(dir_ + "/netd.sock"));

        backend_ = std::make_unique<EthernetBackendNetd>(dir_ + "/sys");
    }

    ~EthernetNetdFixture() {
        backend_.reset();
        server_.reset(); // closes clients, unlinks the socket
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    /// Run get_info() on a worker thread (it blocks on the daemon), wait for
    /// its GET to be recorded, push the reply lines, join, and return the
    /// info it produced. join() happens BEFORE the failure assertion so a
    /// red run can never destroy a joinable thread.
    EthernetInfo query_with_reply(const std::vector<std::string>& lines) {
        const size_t baseline = server_->recorded_line_count();
        EthernetInfo info;
        std::thread caller([this, &info] { info = backend_->get_info(); });
        const bool spoke = wait_until([&] { return server_->recorded_line_count() > baseline; });
        if (spoke) {
            for (const std::string& line : lines)
                server_->push_line(line);
            server_->push_line("OK"); // the ack ends the client's read
        }
        caller.join();
        REQUIRE(spoke);
        return info;
    }

    helix_test::EnvVarGuard sock_env_{"HELIX_NETD_SOCKET"};
    helix_test::EnvVarGuard bin_env_{"HELIX_NETD_BIN"};
    std::unique_ptr<helix_test::NetdFakeServer> server_;
    std::unique_ptr<EthernetBackendNetd> backend_;

  private:
    std::string dir_;
};

// ============================================================================
// 1. MODE=ETHERNET STATE=ONLINE with an address => fully connected info,
//    with the interface and MAC resolved from the fake sysfs tree.
// ============================================================================
TEST_CASE_METHOD(EthernetNetdFixture, "netd ethernet online snapshot maps to connected info",
                 "[netd][ethernet]") {
    const EthernetInfo info = query_with_reply({"MODE=ETHERNET", "STATE=ONLINE", "IP=10.0.0.5"});

    REQUIRE(info.connected);
    REQUIRE(info.ip_address == "10.0.0.5");
    REQUIRE(info.interface == "eth0");                // resolved from the sysfs scan
    REQUIRE(info.mac_address == "aa:bb:cc:12:34:56"); // trimmed from the address file
    REQUIRE(info.status == "Connected");
}

// ============================================================================
// 2. STATE=CONNECTED counts as connected too — connected_state() covers both
//    carrying states, not just ONLINE.
// ============================================================================
TEST_CASE_METHOD(EthernetNetdFixture, "netd ethernet STATE=CONNECTED counts as connected too",
                 "[netd][ethernet]") {
    const EthernetInfo info =
        query_with_reply({"MODE=ETHERNET", "STATE=CONNECTED", "IP=192.168.2.7"});

    REQUIRE(info.connected);
    REQUIRE(info.ip_address == "192.168.2.7");
    REQUIRE(info.status == "Connected");
}

// ============================================================================
// 3. Ethernet mode still waiting for DHCP: disconnected, status names the wait.
// ============================================================================
TEST_CASE_METHOD(EthernetNetdFixture, "netd ethernet DHCP_WAIT reports the wait",
                 "[netd][ethernet]") {
    const EthernetInfo info = query_with_reply({"MODE=ETHERNET", "STATE=DHCP_WAIT", "IP="});

    REQUIRE_FALSE(info.connected);
    REQUIRE(info.ip_address.empty());
    REQUIRE(info.status == "Waiting for address");
}

// ============================================================================
// 4. NO_CARRIER: cable out / link down.
// ============================================================================
TEST_CASE_METHOD(EthernetNetdFixture, "netd ethernet NO_CARRIER reports no carrier",
                 "[netd][ethernet]") {
    const EthernetInfo info = query_with_reply({"MODE=ETHERNET", "STATE=NO_CARRIER"});

    REQUIRE_FALSE(info.connected);
    REQUIRE(info.ip_address.empty());
    REQUIRE(info.status == "No carrier");
}

// ============================================================================
// 5. A carrying state with no address yet is NOT connected.
// ============================================================================
TEST_CASE_METHOD(EthernetNetdFixture, "netd ethernet online without an address is not connected",
                 "[netd][ethernet]") {
    const EthernetInfo info = query_with_reply({"MODE=ETHERNET", "STATE=ONLINE"});

    REQUIRE_FALSE(info.connected);
    REQUIRE(info.ip_address.empty());
    REQUIRE(info.status == "No address yet");
}

// ============================================================================
// 6. WiFi is the active transport: the ethernet row must stay disconnected
//    even though the daemon reports a live connection.
// ============================================================================
TEST_CASE_METHOD(EthernetNetdFixture, "netd wifi-active snapshot keeps ethernet disconnected",
                 "[netd][ethernet]") {
    const EthernetInfo info = query_with_reply({"MODE=WIFI", "STATE=CONNECTED", "IP=10.0.0.4"});

    REQUIRE_FALSE(info.connected);
    REQUIRE(info.ip_address.empty());
    REQUIRE(info.status == "WiFi is the active connection");
}

// ============================================================================
// 7. Daemon unreachable: disconnected with the daemon named in the status,
//    while has_interface() stays true from sysfs — visibility must not flap
//    when the daemon restarts.
// ============================================================================
TEST_CASE_METHOD(EthernetNetdFixture, "netd unreachable daemon leaves sysfs detection intact",
                 "[netd][ethernet]") {
    // Repoint inside the case; the guard restores the fake server's path.
    helix_test::EnvVarGuard dead("HELIX_NETD_SOCKET");
    dead.set("/tmp/helix_netd_eth_no_such_dir/netd.sock");

    const EthernetInfo info = backend_->get_info(); // connect fails instantly

    REQUIRE_FALSE(info.connected);
    REQUIRE(info.ip_address.empty());
    REQUIRE(info.status == "Network daemon unavailable");
    REQUIRE(backend_->has_interface());
}

// ============================================================================
// 8. One-shot contract: every get_info() is its own connection carrying
//    exactly one GET — no SUBSCRIBE, nothing persistent.
// ============================================================================
TEST_CASE_METHOD(EthernetNetdFixture,
                 "netd ethernet get_info is one connection and one GET per call",
                 "[netd][ethernet]") {
    // Three concurrent calls, every reply held until all three connections
    // exist, so connection_count() can observe them simultaneously.
    std::array<EthernetInfo, 3> infos{};
    std::vector<std::thread> callers;
    callers.reserve(infos.size());
    for (size_t i = 0; i < infos.size(); ++i)
        callers.emplace_back([this, &infos, i] { infos[i] = backend_->get_info(); });

    const bool all_spoke = wait_until([&] { return server_->recorded_line_count() >= 3; });
    if (all_spoke) {
        REQUIRE(server_->connection_count() == 3);

        for (const char* line : {"MODE=ETHERNET", "STATE=ONLINE", "IP=10.0.0.9"})
            server_->push_line(line);
        server_->push_line("OK");
    }
    // Join BEFORE any failure assertion: a red run must never unwind past a
    // joinable thread (std::thread's destructor terminates the process).
    for (auto& caller : callers)
        caller.join();
    REQUIRE(all_spoke);

    REQUIRE(server_->recorded_line_count() == 3);
    for (const std::string& line : server_->recorded_lines())
        REQUIRE(line == "GET");

    for (const EthernetInfo& info : infos) {
        REQUIRE(info.connected);
        REQUIRE(info.ip_address == "10.0.0.9");
    }
}

// ============================================================================
// 9. has_interface is pure sysfs classification: lo + wlan0 is not ethernet.
//    No daemon involved — a bare test case with its own tree.
// ============================================================================
TEST_CASE("netd ethernet has_interface is false with only lo and wlan0", "[netd][ethernet]") {
    char dir_template[] = "/tmp/helix_netd_eth_none_XXXXXX";
    char* dir = ::mkdtemp(dir_template);
    REQUIRE(dir != nullptr);
    const std::string root(dir);

    fs::create_directories(root + "/class/net/lo");
    fs::create_directories(root + "/class/net/wlan0");

    EthernetBackendNetd backend(root);
    REQUIRE_FALSE(backend.has_interface());

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ============================================================================
// 10. Name classification edges: an unrecognized-but-ethernet naming scheme
//     (end0, RK3588 boards) counts; virtual bridges and veth pairs never do.
// ============================================================================
TEST_CASE("netd ethernet has_interface accepts end0 and rejects virtual bridges",
          "[netd][ethernet]") {
    char keep_template[] = "/tmp/helix_netd_eth_end0_XXXXXX";
    char* keep = ::mkdtemp(keep_template);
    REQUIRE(keep != nullptr);
    fs::create_directories(std::string(keep) + "/class/net/end0");
    EthernetBackendNetd rockchip(keep);
    REQUIRE(rockchip.has_interface());

    char skip_template[] = "/tmp/helix_netd_eth_virt_XXXXXX";
    char* skip = ::mkdtemp(skip_template);
    REQUIRE(skip != nullptr);
    fs::create_directories(std::string(skip) + "/class/net/docker0");
    fs::create_directories(std::string(skip) + "/class/net/br-lan");
    fs::create_directories(std::string(skip) + "/class/net/veth0");
    EthernetBackendNetd virtual_only(skip);
    REQUIRE_FALSE(virtual_only.has_interface());

    std::error_code ec;
    fs::remove_all(keep, ec);
    fs::remove_all(skip, ec);
}

#endif // !__ANDROID__
