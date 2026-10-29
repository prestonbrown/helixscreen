// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../mocks/mock_mdns_discovery.h"
#include "../test_helpers/update_queue_test_access.h"
#include "lan_sweep.h"
#include "printer_discovery_list.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr uint32_t ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a << 24) | (b << 16) | (c << 8) | d;
}

LocalIpv4 net(uint32_t address, uint32_t netmask) {
    return LocalIpv4{address, netmask};
}

void drain() {
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
}

/// Drain the UI queue until @p done or two seconds pass.
template <typename F> bool pump_until(F done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    drain();
    return true;
}

} // namespace

// ============================================================================
// Which addresses a sweep asks
// ============================================================================

TEST_CASE("LAN sweep: a /24 is every host address, ours included", "[lan_sweep]") {
    const auto hosts = lan_sweep_hosts(net(ip(192, 168, 1, 42), ip(255, 255, 255, 0)));
    REQUIRE(hosts.size() == 254);
    CHECK(hosts.front() == "192.168.1.1");
    CHECK(hosts.back() == "192.168.1.254");
    // A printer running HelixScreen lists its own Moonraker.
    CHECK(std::find(hosts.begin(), hosts.end(), "192.168.1.42") != hosts.end());
    CHECK(std::find(hosts.begin(), hosts.end(), "192.168.1.52") != hosts.end());
}

TEST_CASE("LAN sweep: a wider network is narrowed to our /24", "[lan_sweep]") {
    const auto hosts = lan_sweep_hosts(net(ip(10, 4, 7, 9), ip(255, 0, 0, 0)));
    REQUIRE(hosts.size() == 254);
    CHECK(hosts.front() == "10.4.7.1");
    CHECK(hosts.back() == "10.4.7.254");
    CHECK(lan_sweep_hosts(net(ip(172, 16, 5, 9), ip(255, 255, 0, 0))).size() == 254);
}

TEST_CASE("LAN sweep: a narrower network is kept", "[lan_sweep]") {
    const auto hosts = lan_sweep_hosts(net(ip(192, 168, 1, 42), ip(255, 255, 255, 128)));
    REQUIRE(hosts.size() == 126);
    CHECK(hosts.front() == "192.168.1.1");
    CHECK(hosts.back() == "192.168.1.126");
}

TEST_CASE("LAN sweep: a public or unknown network is not swept", "[lan_sweep]") {
    CHECK(lan_sweep_hosts(net(ip(8, 8, 8, 8), ip(255, 255, 255, 0))).empty());
    CHECK(lan_sweep_hosts(net(ip(172, 32, 0, 5), ip(255, 255, 255, 0))).empty());
    CHECK(lan_sweep_hosts(net(ip(192, 169, 1, 5), ip(255, 255, 255, 0))).empty());
    CHECK(lan_sweep_hosts(net(0, 0)).empty());
}

// ============================================================================
// What counts as Moonraker
// ============================================================================

TEST_CASE("LAN sweep: only Moonraker's /server/info answer counts", "[lan_sweep]") {
    const std::string moonraker = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
                                  R"({"result":{"klippy_connected":true,"klippy_state":"ready",)"
                                  R"("components":["file_manager"],"moonraker_version":"v0.9.3"}})";
    CHECK(is_moonraker_server_info_response(moonraker));

    // A printer firmware that blanks the version still carries Moonraker's fields.
    CHECK(is_moonraker_server_info_response(
        "HTTP/1.0 200 OK\r\n\r\n"
        R"({"result":{"klippy_state":"ready","components":[],"moonraker_version":"?"}})"));

    CHECK_FALSE(is_moonraker_server_info_response(
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<html>Router login</html>"));
    CHECK_FALSE(is_moonraker_server_info_response(
        "HTTP/1.1 404 Not Found\r\n\r\n"
        R"({"result":{"klippy_state":"ready","components":[],"moonraker_version":"x"}})"));
    CHECK_FALSE(is_moonraker_server_info_response("HTTP/1.1 200 OK\r\n\r\n"
                                                  R"({"result":{"status":"ok"}})"));
    CHECK_FALSE(is_moonraker_server_info_response("HTTP/1.1 200 OK\r\n\r\n"
                                                  R"({"result":"moonraker_version"})"));
    CHECK_FALSE(is_moonraker_server_info_response(""));

    // Moonraker behind its login still counts; another server's 401 does not.
    CHECK(is_moonraker_server_info_response(
        "HTTP/1.1 401 Unauthorized\r\n\r\n"
        R"({"error": {"code": 401, "message": "Unauthorized", "traceback": ""}})"));
    CHECK(is_moonraker_server_info_response("HTTP/1.1 403 Forbidden\r\n\r\n"
                                            R"({"error":{"code":403,"message":"Forbidden"}})"));
    CHECK_FALSE(is_moonraker_server_info_response(
        "HTTP/1.1 401 Unauthorized\r\n\r\n<html>Router login</html>"));
    CHECK_FALSE(
        is_moonraker_server_info_response("HTTP/1.1 404 Not Found\r\n\r\n"
                                          R"({"error":{"code":404,"message":"Not Found"}})"));

    // Only the result object's own keys count.
    CHECK_FALSE(
        is_moonraker_server_info_response("HTTP/1.1 200 OK\r\n\r\n"
                                          R"({"result":{"inner":{"moonraker_version":"x"}}})"));
    CHECK_FALSE(is_moonraker_server_info_response(
        "HTTP/1.1 200 OK\r\n\r\n"
        R"({"result":{"inner":{"a":1,"moonraker_version":"x"}}})"));
    CHECK_FALSE(is_moonraker_server_info_response(
        "HTTP/1.1 200 OK\r\n\r\n"
        R"({"result":{"note":"moonraker_version","list":["klippy_state","components"]}})"));
    CHECK_FALSE(
        is_moonraker_server_info_response("HTTP/1.1 200 OK\r\n\r\n"
                                          R"({"other":{"moonraker_version":"x"},"result":{}})"));
    CHECK(is_moonraker_server_info_response(
        "HTTP/1.1 200 OK\r\n\r\n"
        R"({"result":{"note":"a \"quoted\" {brace}","moonraker_version":"v1"}})"));
    CHECK_FALSE(is_moonraker_server_info_response("HTTP/1.1 200 OK\r\n\r\n{\"result\":"));
}

// ============================================================================
// Merging with mDNS, and the dropdown
// ============================================================================

TEST_CASE("Printer discovery: one entry per address, mDNS's name kept", "[lan_sweep]") {
    const std::vector<DiscoveredPrinter> mdns = {
        {"voron", "voron.local", "192.168.1.112", 7125, ""}};
    const std::vector<DiscoveredPrinter> sweep = {{"", "", "192.168.1.112", 7125, ""},
                                                  {"", "", "192.168.1.52", 80, ""},
                                                  {"", "", "192.168.1.52", 80, ""}};
    const auto merged = merge_discovered_printers(mdns, sweep);
    REQUIRE(merged.size() == 2);
    CHECK(merged[0].name == "voron");
    CHECK(merged[1].ip_address == "192.168.1.52");
    CHECK(merged[1].port == 80);

    CHECK(discovered_printer_label(merged[0]) == "voron (192.168.1.112:7125)");
    CHECK(discovered_printer_label(merged[1]) == "192.168.1.52:80");
}

TEST_CASE("Printer discovery: dropdown row 0 is the prompt, row i+1 is printer i", "[lan_sweep]") {
    const std::vector<DiscoveredPrinter> found = {{"", "", "192.168.1.52", 80, ""}};
    CHECK(discovered_printer_options(found, true) ==
          std::string(lv_tr("Choose a discovered printer")) + "\n192.168.1.52:80");
    CHECK(discovered_printer_options({}, true) == lv_tr("Scanning..."));
    CHECK(discovered_printer_options({}, false) == lv_tr("No printers found"));

    CHECK(discovered_printer_at(found, 0) == nullptr);
    REQUIRE(discovered_printer_at(found, 1) != nullptr);
    CHECK(discovered_printer_at(found, 1)->port == 80);
    CHECK(discovered_printer_at(found, 2) == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "Printer discovery: mDNS and sweep updates merge as they arrive",
                 "[lan_sweep]") {
    auto mdns = std::make_unique<MockMdnsDiscovery>();
    auto sweep = std::make_unique<MockMdnsDiscovery>();
    MockMdnsDiscovery* mdns_raw = mdns.get();
    MockMdnsDiscovery* sweep_raw = sweep.get();
    MoonrakerDiscovery discovery(std::move(mdns), std::move(sweep));

    std::vector<std::vector<DiscoveredPrinter>> updates;
    discovery.start_discovery(
        [&](const std::vector<DiscoveredPrinter>& p) { updates.push_back(p); });

    sweep_raw->add_fake_printer("", "", "192.168.1.52", 80);
    sweep_raw->add_fake_printer("", "", "192.168.1.112", 7125);
    sweep_raw->simulate_discovery();
    REQUIRE_FALSE(updates.empty());
    CHECK(updates.back().size() == 2);

    mdns_raw->add_fake_printer("voron", "voron.local", "192.168.1.112", 7125);
    mdns_raw->simulate_discovery();
    REQUIRE(updates.back().size() == 2);
    CHECK(updates.back()[0].name == "voron");
    CHECK(discovery.is_discovering());

    discovery.stop_discovery();
    CHECK_FALSE(discovery.is_discovering());
}

// ============================================================================
// The sweep itself, with the network faked at the probe
// ============================================================================

namespace {

/// A probe that answers as Moonraker on the given host:port pairs and records the load.
struct FakeProbe {
    std::set<std::pair<std::string, uint16_t>> moonraker;
    std::atomic<int> in_flight{0};
    std::atomic<int> max_in_flight{0};
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    std::chrono::milliseconds delay{2};
    std::mutex mu;
    std::vector<std::pair<std::string, uint16_t>> asked;

    bool operator()(const std::string& host, uint16_t port) {
        started++;
        const int now = ++in_flight;
        int prev = max_in_flight.load();
        while (now > prev && !max_in_flight.compare_exchange_weak(prev, now)) {
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            asked.emplace_back(host, port);
        }
        std::this_thread::sleep_for(delay);
        --in_flight;
        const bool hit = moonraker.count({host, port}) > 0;
        finished++;
        return hit;
    }

    bool was_asked(const std::string& host, uint16_t port) {
        std::lock_guard<std::mutex> lock(mu);
        return std::find(asked.begin(), asked.end(), std::make_pair(host, port)) != asked.end();
    }
};

LanSweepDiscovery::LocalNetFn on_192_168_1() {
    return [] { return std::optional<LocalIpv4>(net(ip(192, 168, 1, 42), ip(255, 255, 255, 0))); };
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "LAN sweep: finds Moonraker on 7125 or 80, never more than the cap at once",
                 "[lan_sweep]") {
    FakeProbe probe;
    probe.moonraker = {{"192.168.1.52", 80}, {"192.168.1.112", 7125}, {"192.168.1.112", 80}};
    LanSweepLimits limits;
    limits.max_concurrent = 4;
    LanSweepDiscovery sweep(limits, on_192_168_1(),
                            [&probe](const std::string& h, uint16_t p) { return probe(h, p); });

    std::vector<DiscoveredPrinter> latest;
    int updates = 0;
    sweep.start_discovery([&](const std::vector<DiscoveredPrinter>& p) {
        latest = p;
        ++updates;
    });
    REQUIRE(sweep.is_discovering());
    REQUIRE(pump_until([&] { return !sweep.is_discovering(); }));

    CHECK(probe.max_in_flight.load() <= 4);
    CHECK(probe.max_in_flight.load() >= 2);
    REQUIRE(latest.size() == 2);
    CHECK(std::find(latest.begin(), latest.end(),
                    DiscoveredPrinter{"", "", "192.168.1.52", 80, ""}) != latest.end());
    CHECK(std::find(latest.begin(), latest.end(),
                    DiscoveredPrinter{"", "", "192.168.1.112", 7125, ""}) != latest.end());
    // One update per find, then one when the sweep ends.
    CHECK(updates == 3);

    // Each address is asked 7125 first and 80 only when 7125 is silent.
    CHECK(probe.was_asked("192.168.1.52", 7125));
    CHECK_FALSE(probe.was_asked("192.168.1.112", 80));
    CHECK(probe.was_asked("192.168.1.42", 7125));
    CHECK(probe.started.load() == 254 * 2 - 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "LAN sweep: stopping drops results in flight and asks no more",
                 "[lan_sweep]") {
    std::atomic<bool> release{false};
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    LanSweepLimits limits;
    limits.max_concurrent = 2;
    auto sweep = std::make_unique<LanSweepDiscovery>(
        limits, on_192_168_1(), [&](const std::string&, uint16_t) {
            started++;
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            finished++;
            return true;
        });

    int updates = 0;
    sweep->start_discovery([&](const std::vector<DiscoveredPrinter>&) { ++updates; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (started.load() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(started.load() == 2);

    // The screen closes with two probes running, both about to report Moonraker.
    sweep.reset();
    release = true;
    const auto settle = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (finished.load() < 2 && std::chrono::steady_clock::now() < settle) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(finished.load() == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    drain();

    CHECK(updates == 0);
    CHECK(started.load() == 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "LAN sweep: no private network means no sweep", "[lan_sweep]") {
    int probes = 0;
    LanSweepDiscovery sweep(
        LanSweepLimits{},
        [] { return std::optional<LocalIpv4>(net(ip(8, 8, 8, 8), ip(255, 255, 255, 0))); },
        [&](const std::string&, uint16_t) {
            ++probes;
            return false;
        });
    sweep.start_discovery([](const std::vector<DiscoveredPrinter>&) {});
    CHECK_FALSE(sweep.is_discovering());
    CHECK(probes == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "LAN sweep: a worker rests after every probe", "[lan_sweep]") {
    LanSweepLimits limits;
    limits.max_concurrent = 1;
    limits.pause_between_probes = std::chrono::milliseconds(40);
    std::vector<std::chrono::steady_clock::time_point> asked;
    std::mutex mu;
    // A /30 holds two host addresses, ours among them, each asked on 7125 and then 80.
    LanSweepDiscovery sweep(
        limits,
        [] { return std::optional<LocalIpv4>(net(ip(192, 168, 1, 42), ip(255, 255, 255, 252))); },
        [&](const std::string&, uint16_t) {
            std::lock_guard<std::mutex> lock(mu);
            asked.push_back(std::chrono::steady_clock::now());
            return false;
        });
    sweep.start_discovery([](const std::vector<DiscoveredPrinter>&) {});
    REQUIRE(pump_until([&] { return !sweep.is_discovering(); }));

    std::lock_guard<std::mutex> lock(mu);
    REQUIRE(asked.size() == 4);
    for (size_t i = 1; i < asked.size(); ++i) {
        CHECK(asked[i] - asked[i - 1] >= std::chrono::milliseconds(40));
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "LAN sweep: workers that never start do not hold it open",
                 "[lan_sweep]") {
    const size_t runs = GENERATE(size_t{0}, size_t{1});
    LanSweepLimits limits;
    limits.max_concurrent = 3;
    // Starts `runs` of the bodies asked for, on this thread, and reports only those.
    auto start_some = [runs](size_t, const LanSweepLimits&, std::function<void()> body) {
        for (size_t i = 0; i < runs; ++i) {
            body();
        }
        return LanSweepWorkers{nullptr, runs};
    };
    LanSweepDiscovery sweep(
        limits, on_192_168_1(), [](const std::string&, uint16_t) { return false; }, start_some);
    int updates = 0;
    sweep.start_discovery([&](const std::vector<DiscoveredPrinter>&) { ++updates; });
    drain();

    CHECK_FALSE(sweep.is_discovering());
    CHECK(updates == 1);
}

// ============================================================================
// Test Connection
// ============================================================================

namespace {

/// Runs the test's worker body when the test says so, so a test can act in between.
struct HeldWorker {
    std::function<void()> body;
    StartWorkersFn starter() {
        return [this](size_t, const LanSweepLimits&, std::function<void()> b) {
            body = std::move(b);
            return LanSweepWorkers{nullptr, 1};
        };
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Connection test: stops at the first port Moonraker answers on",
                 "[lan_sweep][connection_test]") {
    std::vector<uint16_t> asked;
    HeldWorker worker;
    set_connection_test_seams_for_test(
        [&asked](const std::string&, uint16_t port, const std::string&) {
            asked.push_back(port);
            MoonrakerProbe answer(port == 80);
            answer.klippy_state = port == 80 ? "ready" : "";
            answer.failure = ConnectFailureKind::Refused;
            return answer;
        },
        worker.starter());
    MoonrakerConnectionTest test;
    std::optional<MoonrakerConnectionTest::Result> got;
    test.start("10.9.9.9", {7125, 80, 8080}, [&](const auto& r) { got = r; });
    REQUIRE(test.running());
    worker.body();
    drain();
    set_connection_test_seams_for_test(nullptr, nullptr);

    REQUIRE(got.has_value());
    CHECK(got->reached);
    CHECK(got->port == 80);
    CHECK(got->klippy_state == "ready");
    CHECK(got->tried == std::vector<int>{7125, 80});
    CHECK(asked == std::vector<uint16_t>{7125, 80});
    CHECK_FALSE(test.running());
    CHECK(connection_test_status("10.9.9.9", *got).find("10.9.9.9:80") != std::string::npos);
}

TEST_CASE_METHOD(LVGLTestFixture, "Connection test: a miss reports every port and the last failure",
                 "[lan_sweep][connection_test]") {
    HeldWorker worker;
    set_connection_test_seams_for_test(
        [](const std::string&, uint16_t port, const std::string&) {
            MoonrakerProbe answer;
            answer.failure = port == 80 ? ConnectFailureKind::Timeout : ConnectFailureKind::Refused;
            return answer;
        },
        worker.starter());
    MoonrakerConnectionTest test;
    std::optional<MoonrakerConnectionTest::Result> got;
    test.start("10.9.9.9", {7125, 80}, [&](const auto& r) { got = r; });
    worker.body();
    drain();
    set_connection_test_seams_for_test(nullptr, nullptr);

    REQUIRE(got.has_value());
    CHECK_FALSE(got->reached);
    CHECK(got->tried == std::vector<int>{7125, 80});
    CHECK(got->failure == ConnectFailureKind::Timeout);
}

TEST_CASE_METHOD(LVGLTestFixture, "Connection test: cancel and a new start drop the old answer",
                 "[lan_sweep][connection_test]") {
    HeldWorker first;
    set_connection_test_seams_for_test(
        [](const std::string&, uint16_t, const std::string&) { return MoonrakerProbe(true); },
        first.starter());
    MoonrakerConnectionTest test;
    int old_answers = 0;
    test.start("10.9.9.1", {7125}, [&](const auto&) { ++old_answers; });
    std::function<void()> stale = first.body;

    SECTION("cancel before the worker ran") {
        test.cancel();
        CHECK_FALSE(test.running());
    }
    SECTION("cancel after the worker answered, before the answer arrived") {
        stale();
        stale = [] {};
        test.cancel();
    }
    SECTION("a second start") {
        HeldWorker second;
        set_connection_test_seams_for_test(
            [](const std::string&, uint16_t, const std::string&) { return MoonrakerProbe(true); },
            second.starter());
        // The first test has answered, and its answer is still on its way, when the second
        // starts.
        stale();
        stale = [] {};
        int new_answers = 0;
        test.start("10.9.9.2", {7125}, [&](const auto&) { ++new_answers; });
        drain();
        CHECK(new_answers == 0);
        second.body();
        drain();
        CHECK(new_answers == 1);
    }
    stale();
    drain();
    set_connection_test_seams_for_test(nullptr, nullptr);
    CHECK(old_answers == 0);
}

TEST_CASE("Connection test: /server/info gives Klipper's state", "[lan_sweep][connection_test]") {
    const MoonrakerProbe probe = read_server_info_response(
        "HTTP/1.1 200 OK\r\n\r\n"
        R"({"result":{"klippy_state":"startup","components":[],"moonraker_version":"v1"}})");
    CHECK(probe.moonraker);
    CHECK(probe.klippy_state == "startup");
    CHECK(read_server_info_response("HTTP/1.1 200 OK\r\n\r\n<html></html>").klippy_state.empty());
}

// ============================================================================
// Moonraker behind its login (prestonbrown/helixscreen#1929)
// ============================================================================

TEST_CASE("Connection test: a 401 from Moonraker is a login it requires", "[lan_sweep][api-key]") {
    const MoonrakerProbe refused =
        read_server_info_response("HTTP/1.1 401 Unauthorized\r\n\r\n"
                                  R"({"error": {"code": 401, "message": "Unauthorized"}})");
    CHECK(refused.moonraker);
    CHECK(refused.login_required);

    const MoonrakerProbe open = read_server_info_response(
        "HTTP/1.1 200 OK\r\n\r\n"
        R"({"result":{"klippy_state":"ready","components":[],"moonraker_version":"v1"}})");
    CHECK(open.moonraker);
    CHECK_FALSE(open.login_required);
}

TEST_CASE("Connection test: the request carries the key only when there is one",
          "[lan_sweep][api-key]") {
    const std::string bare = server_info_request("10.9.9.9", 7125);
    CHECK(bare.rfind("GET /server/info HTTP/1.0\r\n", 0) == 0);
    CHECK(bare.find("X-Api-Key") == std::string::npos);
    CHECK(bare.substr(bare.size() - 4) == "\r\n\r\n");

    const std::string keyed = server_info_request("10.9.9.9", 7125, "abc123");
    CHECK(keyed.find("\r\nX-Api-Key: abc123\r\n") != std::string::npos);
    CHECK(keyed.substr(keyed.size() - 4) == "\r\n\r\n");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Connection test: the key goes to the probe, the refusal comes back",
                 "[lan_sweep][connection_test][api-key]") {
    std::vector<std::string> keys;
    HeldWorker worker;
    set_connection_test_seams_for_test(
        [&keys](const std::string&, uint16_t, const std::string& key) {
            keys.push_back(key);
            MoonrakerProbe answer(true);
            answer.login_required = true;
            return answer;
        },
        worker.starter());
    MoonrakerConnectionTest test;
    std::optional<MoonrakerConnectionTest::Result> got;

    SECTION("with a key: rejected") {
        test.start("10.9.9.9", {7125}, [&](const auto& r) { got = r; }, "k3y");
        worker.body();
        drain();
        REQUIRE(got.has_value());
        CHECK(keys == std::vector<std::string>{"k3y"});
        CHECK(got->key_sent);
        CHECK(connection_test_status("10.9.9.9", *got) ==
              "Moonraker at 10.9.9.9:7125 rejected the API key.");
    }
    SECTION("without one: required") {
        test.start("10.9.9.9", {7125}, [&](const auto& r) { got = r; });
        worker.body();
        drain();
        REQUIRE(got.has_value());
        CHECK(keys == std::vector<std::string>{""});
        CHECK_FALSE(got->key_sent);
        CHECK(connection_test_status("10.9.9.9", *got) ==
              "Moonraker at 10.9.9.9:7125 requires an API key.");
    }
    set_connection_test_seams_for_test(nullptr, nullptr);
    CHECK(got->reached);
    CHECK(got->login_required);
}

TEST_CASE_METHOD(LVGLTestFixture, "API key: blank or 32 hex digits, anything else is explained",
                 "[lan_sweep][api-key]") {
    CHECK(api_key_problem("").empty());
    CHECK(api_key_problem("0123456789abcdef0123456789ABCDEF").empty());
    CHECK_FALSE(api_key_problem("0123456789abcdef0123456789abcde").empty());   // 31
    CHECK_FALSE(api_key_problem("0123456789abcdef0123456789abcdef0").empty()); // 33
    CHECK_FALSE(api_key_problem("0123456789abcdef0123456789abcdeg").empty());  // not hex
}
