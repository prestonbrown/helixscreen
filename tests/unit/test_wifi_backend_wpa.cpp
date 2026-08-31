// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wifi_backend_wpa.cpp
 * @brief Protocol-level tests for WifiBackendWpaSupplicant against a fake
 *        wpa_supplicant control socket (prestonbrown/helixscreen#1407).
 *
 * The backend's control/monitor connections are AF_UNIX SOCK_DGRAM wpa_ctrl
 * handles; the fake (tests/test_helpers/wpa_fake_supplicant.h) is one bound
 * DGRAM endpoint answering the command protocol and pushing unsolicited
 * monitor events. These are the first tests that drive a SUCCESSFUL init of
 * this backend — the #1036 regression used a bound-but-silent socket, which
 * can only pin the failure path.
 *
 * The contract under test is stated on WifiBackend::trigger_scan
 * (include/wifi_backend.h): a success return obligates an eventual
 * SCAN_COMPLETE. The wpa backend's only producer of that event is the
 * unsolicited CTRL-EVENT-SCAN-RESULTS on the monitor connection — when the
 * connection dies mid-scan, or a FAIL-BUSY reply rides an in-flight scan
 * that never completes, nothing else can release the manager's scheduler
 * latch. The watchdog bounds the wait.
 */

#include "../test_helpers/wpa_fake_supplicant.h"
#include "wifi_backend_wpa_supplicant.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr int kWatchdogMs = 250;

struct WpaFakeFixture {
    char dir_template[32];
    std::string dir;
    std::optional<helix_test::WpaFakeSupplicant> fake;

    WpaFakeFixture() {
        std::strcpy(dir_template, "/tmp/helix_wpa_fake_XXXXXX");
        char* d = ::mkdtemp(dir_template);
        REQUIRE(d != nullptr);
        dir = d;
        // Emplaced here, not member-initialized: the fake needs the dir the
        // ctor body just created.
        fake.emplace(dir);
        REQUIRE(fake->start());
        env_guard.emplace(dir);
    }

    ~WpaFakeFixture() {
        env_guard.reset();
        fake.reset();
        ::rmdir(dir.c_str());
    }

    std::optional<helix_test::WpaSocketDirGuard> env_guard;
};

/// Counts SCAN_COMPLETE dispatches from a backend under test.
struct ScanEventCounter {
    std::mutex m;
    std::condition_variable cv;
    int count = 0;

    void register_with(WifiBackend& backend) {
        backend.register_event_callback("SCAN_COMPLETE", [this](const std::string&) {
            std::lock_guard<std::mutex> lock(m);
            ++count;
            cv.notify_all();
        });
    }

    bool wait_for(int target, int timeout_ms = 5000) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [this, target] { return count >= target; });
    }

    int get() {
        std::lock_guard<std::mutex> lock(m);
        return count;
    }
};

} // namespace

TEST_CASE("wpa scan watchdog resolves a scan whose event never comes", "[network][wpa][1407]") {
    WpaFakeFixture fx;
    WifiBackendWpaSupplicant backend(kWatchdogMs);
    REQUIRE(backend.start().success());
    REQUIRE(backend.is_running());

    ScanEventCounter events;
    events.register_with(backend);

    // The daemon accepts the SCAN but the monitor connection is about to die
    // in spirit: no CTRL-EVENT-SCAN-RESULTS will ever arrive. (The fake
    // simply never pushes one.)
    WiFiError result = backend.trigger_scan();
    REQUIRE(result.success());
    // One snapshot: two commands() calls would return two distinct vectors,
    // and mixing their iterators is UB.
    const std::vector<std::string> cmds = fx.fake->commands();
    REQUIRE(1 == std::count(cmds.begin(), cmds.end(), "SCAN"));

    // Pre-#1407 this wait timed out forever: nothing else in the backend
    // could discharge the obligation, and the manager's scheduler latched.
    REQUIRE(events.wait_for(1, 10 * kWatchdogMs));
}

TEST_CASE("wpa SCAN_RESULTS event disarms the watchdog", "[network][wpa][1407]") {
    WpaFakeFixture fx;
    WifiBackendWpaSupplicant backend(kWatchdogMs);
    REQUIRE(backend.start().success());

    ScanEventCounter events;
    events.register_with(backend);

    REQUIRE(backend.trigger_scan().success());

    // The real resolution path: the monitor event arrives long before the
    // bound.
    fx.fake->push_event("<3>CTRL-EVENT-SCAN-RESULTS \n");
    REQUIRE(events.wait_for(1, kWatchdogMs));

    // Past the watchdog bound the completion must not fire a second time.
    std::this_thread::sleep_for(std::chrono::milliseconds(4 * kWatchdogMs));
    REQUIRE(events.get() == 1);
}

TEST_CASE("wpa stop with a scan outstanding stays silent and reusable", "[network][wpa][1407]") {
    WpaFakeFixture fx;
    WifiBackendWpaSupplicant backend(kWatchdogMs);
    REQUIRE(backend.start().success());

    ScanEventCounter events;
    events.register_with(backend);

    // A scan is accepted and left unresolved — outstanding when stop() lands.
    REQUIRE(backend.trigger_scan().success());

    backend.stop();
    REQUIRE_FALSE(backend.is_running());

    // No synthetic completion from teardown: the owner resolves the scheduler
    // when IT stops the backend (the #1405 division). The wpa event loop
    // deliberately stays alive across stop(), so an uncleared watchdog would
    // fire here — this is the assertion that catches it.
    std::this_thread::sleep_for(std::chrono::milliseconds(4 * kWatchdogMs));
    REQUIRE(events.get() == 0);

    // The internal single-flight flag was cleared: after start() reuse, a
    // new scan is accepted and completes normally.
    REQUIRE(backend.start().success());
    REQUIRE(backend.trigger_scan().success());
    fx.fake->push_event("<3>CTRL-EVENT-SCAN-RESULTS \n");
    REQUIRE(events.wait_for(1));
}
