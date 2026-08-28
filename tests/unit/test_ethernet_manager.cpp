// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ethernet_backend.h"
#include "ethernet_manager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Info Retrieval Tests
// ============================================================================

TEST_CASE("Ethernet Manager: get_info returns valid struct", "[network][info]") {
    EthernetManager manager;

    EthernetInfo info = manager.get_info();

    // Should return a valid info struct
    REQUIRE(!info.status.empty()); // Status should not be empty

    // If connected, should have valid IP
    if (info.connected) {
        REQUIRE(!info.ip_address.empty());
        REQUIRE(!info.interface.empty());
    }
}

// ============================================================================
// IP Address Retrieval Tests
// ============================================================================

TEST_CASE("Ethernet Manager: get_ip_address behavior", "[network][ip]") {
    EthernetManager manager;

    std::string ip = manager.get_ip_address();

    // Should return empty string if not connected, valid IP if connected
    if (ip.empty()) {
        // Not connected - verify get_info also shows not connected
        EthernetInfo info = manager.get_info();
        REQUIRE(info.connected == false);
    } else {
        // Connected - verify IP format
        // Basic check: should contain dots for IPv4
        bool has_dots = (ip.find('.') != std::string::npos);
        bool has_colons = (ip.find(':') != std::string::npos);

        // Should be either IPv4 (dots) or IPv6 (colons)
        REQUIRE((has_dots || has_colons));

        // Verify get_info also shows connected
        EthernetInfo info = manager.get_info();
        REQUIRE(info.connected == true);
        REQUIRE(info.ip_address == ip);
    }
}

// ============================================================================
// Mock Backend Tests (if using mock)
// ============================================================================

#ifdef USE_MOCK_ETHERNET
TEST_CASE("Ethernet Manager: Mock backend returns expected values", "[network][mock]") {
    EthernetManager manager;

    SECTION("Mock has interface") {
        REQUIRE(manager.has_interface() == true);
    }

    SECTION("Mock returns mock IP") {
        EthernetInfo info = manager.get_info();
        REQUIRE(info.connected == true);
        REQUIRE(info.ip_address == "192.168.1.100");
        REQUIRE(info.interface_name == "eth0");
    }

    SECTION("Mock get_ip_address") {
        std::string ip = manager.get_ip_address();
        REQUIRE(ip == "192.168.1.100");
    }
}
#endif

// ============================================================================
// Edge Cases and Error Handling
// ============================================================================

TEST_CASE("Ethernet Manager: Multiple info queries", "[network][info]") {
    EthernetManager manager;

    // Should handle multiple queries without issues
    EthernetInfo info1 = manager.get_info();
    EthernetInfo info2 = manager.get_info();
    EthernetInfo info3 = manager.get_info();

    // Results should be consistent
    REQUIRE(info1.connected == info2.connected);
    REQUIRE(info2.connected == info3.connected);

    if (info1.connected) {
        REQUIRE(info1.ip_address == info2.ip_address);
        REQUIRE(info2.ip_address == info3.ip_address);
    }
}

TEST_CASE("Ethernet Manager: Repeated interface checks", "[network][interface]") {
    EthernetManager manager;

    bool result1 = manager.has_interface();
    bool result2 = manager.has_interface();
    bool result3 = manager.has_interface();

    // Results should be consistent
    REQUIRE(result1 == result2);
    REQUIRE(result2 == result3);
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST_CASE("Ethernet Manager: Interface and info consistency", "[network][integration]") {
    EthernetManager manager;

    bool has_interface = manager.has_interface();
    EthernetInfo info = manager.get_info();

    if (has_interface) {
        // If we have an interface, info should not indicate backend error
        REQUIRE(info.status != "Backend error");
    }
}

// ============================================================================
// Async API Tests (proposed — must not block UI thread)
// ============================================================================
//
// The synchronous get_info() path runs libhv ifconfig() + per-interface sysfs
// reads which can stall the caller when NetworkManager / the kernel is wonky.
// EthernetManager must expose a fire-and-return async variant that dispatches
// the probe to a worker and delivers the EthernetInfo via callback.
//
// Contract the tests lock in:
//   - get_info_async(cb) returns to the caller in well under 10 ms
//     (the blocking work must NOT run on the calling thread).
//   - The callback eventually fires with a populated EthernetInfo.
//   - The callback is invoked from a thread other than the caller OR via a
//     queued dispatch — either way, it must not be invoked synchronously
//     inside get_info_async() (that would defeat the whole purpose).

TEST_CASE("EthernetManager: async get_info returns without blocking", "[network][async][slow]") {
    EthernetManager manager;

    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> fired{false};
    EthernetInfo received;
    std::thread::id caller_tid = std::this_thread::get_id();
    std::atomic<bool> fired_on_caller_synchronously{false};

    auto t0 = std::chrono::steady_clock::now();

    // NEW API — does not exist yet; this call should fail to compile
    // against current EthernetManager (which has only synchronous get_info()).
    manager.get_info_async([&](const EthernetInfo& info) {
        // If this runs before get_info_async() returns AND on the caller
        // thread, the implementation is still synchronous — flag it.
        if (!fired.load() && std::this_thread::get_id() == caller_tid) {
            // fired is still false here — set a sentinel only if we detect
            // synchronous invocation on the caller thread during the call.
            // (We set 'fired' below, after this check, so the first time
            // through this branch indicates a synchronous in-line fire.)
            fired_on_caller_synchronously.store(true);
        }
        {
            std::lock_guard<std::mutex> lock(m);
            received = info;
            fired.store(true);
            // Notify while still holding the lock. Released first, the waiter can
            // observe the flag, return, and destroy the condition variable while
            // this thread is still inside notify_all() - destroying a condition
            // variable another thread is signalling is undefined behaviour, and
            // TSan reports it as a race on pthread_cond_destroy.
            cv.notify_all();
        }
    });

    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();

    // The call must return quickly — the blocking probe must be off-thread.
    // 10 ms is generous; a synchronous libhv ifconfig() + sysfs read typically
    // takes noticeably longer on a wonky host and is the whole bug.
    REQUIRE(elapsed_ms < 10);

    // The callback must NOT have been invoked synchronously on the caller
    // thread before get_info_async() returned.
    REQUIRE_FALSE(fired_on_caller_synchronously.load());

    // The callback must fire eventually with a populated info struct.
    {
        std::unique_lock<std::mutex> lock(m);
        bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&]() { return fired.load(); });
        REQUIRE(ok);
    }
    REQUIRE_FALSE(received.status.empty());
}

// Regression test for the use-after-free where the detached worker thread in
// get_info_async() captured a raw EthernetBackend* owned by EthernetManager's
// unique_ptr. If EthernetManager was destroyed while the probe was in flight
// (e.g. network settings overlay teardown calling ethernet_manager_.reset()
// with a probe still running), the backend was freed and the worker
// segfaulted. Holding the backend as shared_ptr and capturing it by value
// into the worker lambda lets it outlive the manager.
TEST_CASE("EthernetManager: destroying manager during in-flight async probe is safe",
          "[network][async][slow]") {
    std::atomic<bool> fired{false};
    EthernetInfo received;

    {
        EthernetManager manager;
        manager.get_info_async([&](const EthernetInfo& info) {
            received = info;
            fired.store(true);
        });
        // Immediately destroy the manager while the worker is (likely) still
        // inside backend->get_info(). If the backend were a raw pointer
        // captured from unique_ptr, this would trigger use-after-free.
    }

    // Give the detached worker up to 5 s to finish probing and invoke the
    // callback. If the use-after-free is reintroduced, this process typically
    // dies with SIGSEGV well before the timeout and the test fails hard.
    for (int i = 0; i < 500 && !fired.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(fired.load());
    REQUIRE_FALSE(received.status.empty());
}

TEST_CASE("Ethernet Manager: IP address and info consistency", "[network][integration]") {
    EthernetManager manager;

    std::string ip = manager.get_ip_address();
    EthernetInfo info = manager.get_info();

    // IP from get_ip_address() should match info.ip_address if connected
    if (info.connected) {
        REQUIRE(ip == info.ip_address);
    } else {
        REQUIRE(ip.empty());
    }
}
