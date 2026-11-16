// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "mdns_discovery.h"
#include "utils/network_validation.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

/**
 * @file lan_sweep.h
 * @brief Finding Moonraker on the local network by asking every address
 *
 * Moonraker advertises over mDNS only with [zeroconf] enabled, so most printers never
 * answer a browse. The sweep asks each address of our own /24 for /server/info on the
 * ports a connection test would try (7125, then 80), and reports the ones that answer as
 * Moonraker. MoonrakerDiscovery runs it beside mDNS and merges the two.
 *
 * The same /server/info check is the connection screens' Test Connection
 * (MoonrakerConnectionTest), which never touches the app's live client.
 */
namespace helix {

/// This machine's IPv4 address and netmask on the interface that carries its traffic.
struct LocalIpv4 {
    uint32_t address = 0; ///< Host byte order
    uint32_t netmask = 0; ///< Host byte order
};

/// The addresses a sweep from @p local asks, as dotted quads in ascending order.
///
/// Only a private network (10/8, 172.16/12, 192.168/16) is swept, and never more than the
/// /24 holding our address: a wider netmask is narrowed to it, a narrower one is kept. The
/// network and broadcast addresses are left out; our own address is asked like any other,
/// so a Moonraker on this machine is listed too.
std::vector<std::string> lan_sweep_hosts(const LocalIpv4& local);

/// The interface address the kernel routes outbound traffic from, with its netmask.
/// Empty when there is no IPv4 route.
std::optional<LocalIpv4> active_local_ipv4();

/// What one /server/info request learned.
struct MoonrakerProbe {
    /// A bare answer, as a test's stand-in probe gives it.
    MoonrakerProbe(bool found = false) : moonraker(found) {}

    bool moonraker = false; ///< Moonraker answered
    /// It answered 401 or 403: Moonraker behind its login, refusing this request.
    bool login_required = false;
    std::string klippy_state; ///< Its `result.klippy_state`, when it gave one
    /// Why not, when it did not: nothing answered in time, the port refused, or something
    /// other than Moonraker answered (Unknown).
    ConnectFailureKind failure = ConnectFailureKind::Unknown;
};

/// Reads @p http_response (status line, headers and body) as an answer to GET /server/info.
/// It is Moonraker's when it is a 2xx whose JSON body's top-level `result` object carries
/// Moonraker's own keys, or a 401/403 whose top-level `error` object carries `code` and
/// `message` (Moonraker behind its login). A router's or web server's page on port 80 is not.
MoonrakerProbe read_server_info_response(const std::string& http_response);

/// read_server_info_response(@p http_response).moonraker
bool is_moonraker_server_info_response(const std::string& http_response);

/// The GET /server/info request a probe sends, with @p api_key in Moonraker's key header
/// when it is not empty.
std::string server_info_request(const std::string& host, uint16_t port,
                                const std::string& api_key = "");

/**
 * @brief Ask @p host:@p port for /server/info over plain HTTP and read the answer
 *
 * The one "is this Moonraker" check, for the sweep and for Test Connection. Blocking; run
 * it on a worker thread.
 *
 * @param connect_timeout How long the TCP connect may take
 * @param total_timeout   How long the whole exchange may take
 */
MoonrakerProbe probe_moonraker_http(const std::string& host, uint16_t port,
                                    std::chrono::milliseconds connect_timeout,
                                    std::chrono::milliseconds total_timeout,
                                    const std::string& api_key = "");

/// How hard a sweep pushes the network. lan_sweep_default_limits() gives each platform's.
struct LanSweepLimits {
    /// Probes (and worker threads) in flight at once; small boards run near their thread limit.
    size_t max_concurrent = 12;
    std::chrono::milliseconds connect_timeout{400}; ///< Per probe
    std::chrono::milliseconds total_timeout{1500};  ///< Per probe
    /// A worker's rest after each probe, so a weak radio and CPU stay with the UI.
    std::chrono::milliseconds pause_between_probes{0};
    /// Each worker's stack, where the platform sizes its threads.
    size_t worker_stack_bytes = 16 * 1024;
};

// ----------------------------------------------------------------------------
// Platform pieces: one definition per platform (src/network/lan_sweep_posix.cpp
// on Linux and macOS, the firmware's helixapp/lan_sweep_esp.cpp on the ESP32).
// ----------------------------------------------------------------------------

/// Printer discovery on the real network: mDNS plus the LAN sweep, or the sweep alone
/// where there is no mDNS.
std::unique_ptr<IMdnsDiscovery> make_moonraker_discovery();

/// The sweep's limits on this platform.
LanSweepLimits lan_sweep_default_limits();

/// Workers started for one sweep.
struct LanSweepWorkers {
    /// Holds the workers while the sweep runs; dropping it never waits for a running body.
    std::shared_ptr<void> keepalive;
    size_t started = 0; ///< Bodies that will run; fewer than asked when threads ran out
};

/// Run @p body once on each of @p count new workers.
LanSweepWorkers start_lan_sweep_workers(size_t count, const LanSweepLimits& limits,
                                        std::function<void()> body);

/// Blocking "is Moonraker on host:port" check; runs on a worker.
using MoonrakerProbeFn = std::function<MoonrakerProbe(const std::string& host, uint16_t port)>;
/// The same check sending an API key, which Test Connection may carry; empty sends none.
using KeyedMoonrakerProbeFn = std::function<MoonrakerProbe(const std::string& host, uint16_t port,
                                                           const std::string& api_key)>;
/// How workers start; start_lan_sweep_workers() unless a test stands in.
using StartWorkersFn =
    std::function<LanSweepWorkers(size_t, const LanSweepLimits&, std::function<void()>)>;

/**
 * @brief Test Connection: is Moonraker at this host, and on which port
 *
 * Asks /server/info on each of the ports in turn (moonraker_ports_to_try() gives them) on
 * one worker and reports on the main thread. The app's live client is never touched, so
 * the screen behind the test keeps its printer. One test runs at a time: start() cancels
 * the one before, and cancel() or destruction drops a result still in flight.
 */
class MoonrakerConnectionTest {
  public:
    struct Result {
        bool reached = false;
        int port = 0; ///< Where Moonraker answered, when reached
        /// Moonraker answered but refused the request: it needs an API key, or the one
        /// sent (key_sent) is wrong.
        bool login_required = false;
        bool key_sent = false;    ///< The test carried an API key
        std::string klippy_state; ///< What it said of Klipper, when it said
        std::vector<int> tried;   ///< The ports asked, in order
        ConnectFailureKind failure = ConnectFailureKind::Unknown; ///< The last port's, when not
    };
    using DoneFn = std::function<void(const Result&)>;

    /// The real network, unless set_connection_test_seams_for_test() installed stand-ins.
    MoonrakerConnectionTest();
    ~MoonrakerConnectionTest();

    MoonrakerConnectionTest(const MoonrakerConnectionTest&) = delete;
    MoonrakerConnectionTest& operator=(const MoonrakerConnectionTest&) = delete;

    /// Main thread only. A non-empty @p api_key goes to that host in Moonraker's key header.
    void start(const std::string& host, std::vector<int> ports, DoneFn on_done,
               const std::string& api_key = "");
    /// Main thread only. Does not wait for a running probe.
    void cancel();
    bool running() const {
        return running_;
    }

    struct Run;

  private:
    static void run_on_worker(const std::shared_ptr<Run>& run);
    void finish(const Result& result);

    std::shared_ptr<Run> run_; ///< The test in flight
    std::shared_ptr<void> worker_;
    DoneFn on_done_;
    bool running_ = false;
    AsyncLifetimeGuard lifetime_;
};

/// Stand-ins every MoonrakerConnectionTest uses from its next start(); empty functions
/// restore the real network.
void set_connection_test_seams_for_test(KeyedMoonrakerProbeFn probe, StartWorkersFn start_workers);

/// The translated status line for a failed test: the hint for @p kind, followed by the
/// ports tried when there was more than one.
std::string connect_failure_status(ConnectFailureKind kind, const std::vector<int>& ports_tried);

/// The translated status line for a test that reached Moonraker at @p host: what Klipper
/// is doing, or that the printer needs an API key, or refused the one sent.
std::string connection_test_status(const std::string& host,
                                   const MoonrakerConnectionTest::Result& result);

/// Why @p api_key (already trimmed) cannot be used, translated, or empty when it can. A
/// blank key is usable: it means the printer takes none.
std::string api_key_problem(const std::string& api_key);

/**
 * @brief An IMdnsDiscovery that sweeps the local /24 once per start
 *
 * Each address is asked on the ports moonraker_ports_to_try() gives an untouched default,
 * in order, and counts once, on the first port that answers. Results reach the callback on
 * the main thread as they are found, and once more when the sweep finishes. stop_discovery()
 * (or destruction) cancels what has not run and drops every result still in flight; a probe
 * already running finishes on its own within its total timeout.
 */
class LanSweepDiscovery : public IMdnsDiscovery {
  public:
    using ProbeFn = MoonrakerProbeFn;
    /// Where the sweep runs from; empty skips the sweep.
    using LocalNetFn = std::function<std::optional<LocalIpv4>()>;

    /// The real network under this platform's limits.
    LanSweepDiscovery();
    /// The real network: active_local_ipv4() and probe_moonraker_http() under @p limits.
    explicit LanSweepDiscovery(LanSweepLimits limits);
    LanSweepDiscovery(LanSweepLimits limits, LocalNetFn local_net, ProbeFn probe,
                      StartWorkersFn start_workers = start_lan_sweep_workers);
    ~LanSweepDiscovery() override;

    LanSweepDiscovery(const LanSweepDiscovery&) = delete;
    LanSweepDiscovery& operator=(const LanSweepDiscovery&) = delete;

    /// Main thread only.
    void start_discovery(DiscoveryCallback on_update) override;
    /// Main thread only. Does not wait for running probes.
    void stop_discovery() override;
    bool is_discovering() const override;
    std::vector<DiscoveredPrinter> get_discovered_printers() const override;

    struct Sweep;

  private:
    static void run_on_worker(const std::shared_ptr<Sweep>& sweep);
    void on_found(const std::string& host, uint16_t port);
    void on_finished();

    LanSweepLimits limits_;
    LocalNetFn local_net_;
    ProbeFn probe_;
    StartWorkersFn start_workers_;
    std::shared_ptr<Sweep> sweep_;  ///< The running sweep; main thread only
    std::shared_ptr<void> workers_; ///< LanSweepWorkers::keepalive of the running sweep
    DiscoveryCallback on_update_;
    std::vector<DiscoveredPrinter> found_; ///< Main thread only
    bool discovering_ = false;             ///< Main thread only
    AsyncLifetimeGuard lifetime_;
};

/**
 * @brief mDNS and the LAN sweep together, as one list
 *
 * Both run from start_discovery(); each update from either merges the two lists with
 * merge_discovered_printers(). is_discovering() is true while the sweep runs (mDNS browses
 * for as long as it is started, so it says nothing about being done).
 */
class MoonrakerDiscovery : public IMdnsDiscovery {
  public:
    MoonrakerDiscovery(std::unique_ptr<IMdnsDiscovery> mdns, std::unique_ptr<IMdnsDiscovery> sweep);
    ~MoonrakerDiscovery() override;

    void start_discovery(DiscoveryCallback on_update) override;
    void stop_discovery() override;
    bool is_discovering() const override;
    std::vector<DiscoveredPrinter> get_discovered_printers() const override;

  private:
    void publish();

    std::unique_ptr<IMdnsDiscovery> mdns_;
    std::unique_ptr<IMdnsDiscovery> sweep_;
    std::vector<DiscoveredPrinter> mdns_found_;
    std::vector<DiscoveredPrinter> sweep_found_;
    DiscoveryCallback on_update_;
};

} // namespace helix
