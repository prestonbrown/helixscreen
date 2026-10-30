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

// Subject TU is Linux-only (wifi_backend_wpa_supplicant.h guards __APPLE__ too).
#if !defined(__APPLE__)

#include "../test_helpers/wpa_fake_supplicant.h"
#include "netd_test_server.h" // helix_test::EnvVarGuard
#include "wifi_backend_wpa_supplicant.h"
#include "wifi_regdomain.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

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
        // Hermetic hardware: without this the suite needs a real wlan-named
        // interface, which CI runners do not have.
        net_sysfs_guard.emplace(dir);
        proc_root_guard.emplace(dir);
        // Hermetic regulatory domain: no iw unless a case installs a fake one.
        helix::wifi::regdomain::set_iw_for_test("");
    }

    ~WpaFakeFixture() {
        helix::wifi::regdomain::reset_iw_for_test();
        proc_root_guard.reset();
        net_sysfs_guard.reset();
        env_guard.reset();
        fake.reset();
        ::rmdir(dir.c_str());
    }

    std::optional<helix_test::WpaSocketDirGuard> env_guard;
    std::optional<helix_test::WpaNetSysfsGuard> net_sysfs_guard;
    std::optional<helix_test::WpaProcRootGuard> proc_root_guard;
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

TEST_CASE("wpa reads the configured country, and FAIL as none", "[network][wpa][wifi_country]") {
    WpaFakeFixture fx;
    WifiBackendWpaSupplicant backend(kWatchdogMs);
    REQUIRE(backend.start().success());

    std::string code = "stale";
    fx.fake->set_reply("GET country", "DE");
    REQUIRE(backend.get_country(code).success());
    CHECK(code == "DE");

    // wpa_supplicant answers FAIL when no country= is configured: the world domain.
    fx.fake->set_reply("GET country", "FAIL\n");
    REQUIRE(backend.get_country(code).success());
    CHECK(code.empty());
}

TEST_CASE("wpa reports the kernel's domain when iw can read it", "[network][wpa][wifi_country]") {
    WpaFakeFixture fx;
    WifiBackendWpaSupplicant backend(kWatchdogMs);
    CHECK_FALSE(backend.supports_country());
    REQUIRE(backend.start().success());
    CHECK(backend.supports_country());

    // A domain set outside wpa_supplicant (cfg80211's ieee80211_regdom): its
    // own config has none, the kernel has DE.
    const std::string iw = fx.dir + "/fake_iw";
    {
        std::FILE* f = std::fopen(iw.c_str(), "w");
        REQUIRE(f != nullptr);
        std::fputs("#!/bin/sh\n[ \"$1 $2\" = \"reg get\" ] || exit 1\n"
                   "printf 'global\\ncountry DE: DFS-ETSI\\n'\n",
                   f);
        std::fclose(f);
        REQUIRE(::chmod(iw.c_str(), 0755) == 0);
    }
    helix::wifi::regdomain::set_iw_for_test(iw);
    fx.fake->set_reply("GET country", "FAIL\n");

    std::string code;
    REQUIRE(backend.get_country(code).success());
    CHECK(code == "DE");

    helix::wifi::regdomain::set_iw_for_test("");
    ::unlink(iw.c_str());
}

TEST_CASE("wpa set_country sends SET country, then SAVE_CONFIG", "[network][wpa][wifi_country]") {
    WpaFakeFixture fx;
    WifiBackendWpaSupplicant backend(kWatchdogMs);
    REQUIRE(backend.start().success());

    REQUIRE(backend.set_country("us").success());
    const std::vector<std::string> cmds = fx.fake->commands();
    const auto set = std::find(cmds.begin(), cmds.end(), "SET country US");
    REQUIRE(set != cmds.end());
    CHECK(std::find(set, cmds.end(), "SAVE_CONFIG") != cmds.end());

    // A code outside ISO 3166 never reaches the control socket.
    const size_t before = fx.fake->commands().size();
    CHECK(backend.set_country("ZZ").result == WiFiResult::INVALID_PARAMETERS);
    CHECK(fx.fake->commands().size() == before);
}

TEST_CASE("wpa REGDOM-CHANGE reaches the manager as REGDOM_CHANGE",
          "[network][wpa][wifi_country]") {
    // Declared before the backend so they outlive its event thread.
    std::mutex m;
    std::condition_variable cv;
    std::string seen;

    WpaFakeFixture fx;
    WifiBackendWpaSupplicant backend(kWatchdogMs);
    REQUIRE(backend.start().success());

    backend.register_event_callback("REGDOM_CHANGE", [&](const std::string& data) {
        std::lock_guard<std::mutex> lock(m);
        seen = data;
        cv.notify_all();
    });

    fx.fake->push_event("<3>CTRL-EVENT-REGDOM-CHANGE init=USER type=COUNTRY alpha2=US");
    std::unique_lock<std::mutex> lock(m);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return !seen.empty(); }));
    CHECK(seen.find("alpha2=US") != std::string::npos);
}

// A hidden AP broadcasts no SSID, so it never matches the supplicant's
// beacon-based scan selection: the join only completes when the backend asks
// for a directed probe via scan_ssid=1. The visible join must not ask, or
// every ordinary connect pays a full-channel probe sweep.
// Vendor firmwares relocate the control interface (-O); the backend finds it
// from the daemon's command line in the process table it was pointed at.
TEST_CASE("wpa finds a relocated control directory from the daemon's -O", "[network][wpa]") {
    WpaFakeFixture fx;
    fx.env_guard.reset(); // no HELIX_WPA_SOCKET_DIR: the -O path is the only route
    const std::string pid_dir = fx.dir + "/proc/4242";
    REQUIRE(std::filesystem::create_directories(pid_dir));
    {
        std::FILE* f = std::fopen((pid_dir + "/cmdline").c_str(), "wb");
        REQUIRE(f != nullptr);
        const std::string argv = std::string("/usr/sbin/wpa_supplicant") + '\0' + "-B" + '\0' +
                                 "-O" + '\0' + fx.dir + '\0';
        std::fwrite(argv.data(), 1, argv.size(), f);
        std::fclose(f);
    }

    WifiBackendWpaSupplicant backend(kWatchdogMs);
    CHECK(backend.start().success());
    backend.stop();
}

// The standard control directory is root:netdev 0750, so a service user
// outside netdev cannot list it. That is a permission problem with a fix the
// user can apply, not a missing daemon.
TEST_CASE("wpa reports an unreadable control directory as permission denied", "[network][wpa]") {
    if (::geteuid() == 0) {
        SKIP("root reads a mode-0000 directory, so the denial cannot be staged");
    }
    // The backend always also searches the standard system directories, so a
    // host running a real supplicant would answer with its socket instead.
    for (const char* system_dir :
         {"/run/wpa_supplicant", "/var/run/wpa_supplicant", "/etc/wifi/wpa_supplicant/sockets"}) {
        if (std::filesystem::exists(system_dir)) {
            SKIP("host has a real wpa_supplicant control directory: " << system_dir);
        }
    }
    WpaFakeFixture fx;
    const std::string locked = fx.dir + "/locked";
    REQUIRE(std::filesystem::create_directory(locked));
    REQUIRE(::chmod(locked.c_str(), 0) == 0);
    fx.env_guard.emplace(locked);

    WifiBackendWpaSupplicant backend(kWatchdogMs);
    backend.set_silent(true);
    WiFiError result = backend.start();
    ::chmod(locked.c_str(), 0700);
    ::rmdir(locked.c_str());

    CHECK(result.result == WiFiResult::PERMISSION_DENIED);
}

namespace {
/// A wlan rfkill switch at <base>/sys/class/rfkill/rfkill0 whose `soft` file
/// reads `soft` and cannot be written, like the root-owned 0644 sysfs file a
/// non-root service user sees. The fixture's sysfs guard removes it; this only
/// restores write permission first.
struct ReadOnlyRfkill {
    std::string dir;
    ReadOnlyRfkill(const std::string& base, const char* soft) {
        dir = base + "/sys/class/rfkill/rfkill0";
        REQUIRE(std::filesystem::create_directories(dir));
        std::FILE* t = std::fopen((dir + "/type").c_str(), "w");
        REQUIRE(t != nullptr);
        std::fputs("wlan\n", t);
        std::fclose(t);
        std::FILE* f = std::fopen((dir + "/soft").c_str(), "w");
        REQUIRE(f != nullptr);
        std::fputs(soft, f);
        std::fclose(f);
        REQUIRE(::chmod((dir + "/soft").c_str(), 0444) == 0);
    }
    ~ReadOnlyRfkill() {
        ::chmod((dir + "/soft").c_str(), 0644);
    }
};
} // namespace

// Radio off on an unwritable switch is association-only, so radio on must be
// too: refusing it leaves the toggle stuck off for good.
TEST_CASE("wpa radio on succeeds over an unwritable rfkill switch that is not blocked",
          "[network][wpa][radio]") {
    if (::geteuid() == 0) {
        SKIP("root writes a mode-0444 file, so the unwritable switch cannot be staged");
    }
    WpaFakeFixture fx;
    ReadOnlyRfkill rfkill(fx.dir, "0\n");

    WifiBackendWpaSupplicant backend(kWatchdogMs);
    REQUIRE(backend.start().success());

    REQUIRE(backend.set_radio_enabled(false).success());
    REQUIRE_FALSE(backend.is_radio_enabled());

    WiFiError on = backend.set_radio_enabled(true);
    CHECK(on.success());
    CHECK(backend.is_radio_enabled());
    backend.stop();
}

TEST_CASE("wpa radio on still refuses a switch that stays soft-blocked", "[network][wpa][radio]") {
    if (::geteuid() == 0) {
        SKIP("root writes a mode-0444 file, so the unwritable switch cannot be staged");
    }
    WpaFakeFixture fx;
    ReadOnlyRfkill rfkill(fx.dir, "1\n");

    WifiBackendWpaSupplicant backend(kWatchdogMs);
    REQUIRE(backend.start().success());

    WiFiError on = backend.set_radio_enabled(true);
    CHECK(on.result == WiFiResult::RF_KILL_BLOCKED);
    CHECK_FALSE(backend.is_radio_enabled());
    backend.stop();
}

TEST_CASE("wpa hidden join sends scan_ssid 1; a visible join omits it", "[network][wpa][hidden]") {
    WpaFakeFixture fx;

    // Arm before start(): the backend reconciles HelixScreen's own network
    // store at start, and the connect path saves credentials there when
    // SAVE_CONFIG cannot be verified against the fake. Pointing it at a
    // throwaway dir keeps both off the developer's real config dir.
    const std::string cfg_dir = fx.dir + "/cfg";
    std::filesystem::create_directories(cfg_dir);
    helix_test::EnvVarGuard config_dir("HELIX_CONFIG_DIR");
    config_dir.set(cfg_dir);

    WifiBackendWpaSupplicant backend(kWatchdogMs);
    REQUIRE(backend.start().success());
    REQUIRE(backend.is_running());

    REQUIRE(backend.connect_network("StealthNet", "pw12345", /*is_hidden=*/true).success());
    const std::vector<std::string> hidden_cmds = fx.fake->commands();
    REQUIRE(1 == std::count(hidden_cmds.begin(), hidden_cmds.end(), "SET_NETWORK 0 scan_ssid 1"));

    // commands() accumulates across both joins, so the visible assertions
    // look only at what arrived after the hidden join's slice.
    const size_t after_hidden = fx.fake->commands().size();
    REQUIRE(backend.connect_network("VisibleNet", "pw12345", /*is_hidden=*/false).success());
    const std::vector<std::string> all_cmds = fx.fake->commands();
    const std::vector<std::string> visible_cmds(all_cmds.begin() + static_cast<long>(after_hidden),
                                                all_cmds.end());
    CHECK(0 == std::count(visible_cmds.begin(), visible_cmds.end(), "SET_NETWORK 0 scan_ssid 1"));
    // The visible join really ran end to end, so the zero above is not an
    // empty slice.
    REQUIRE(1 == std::count(visible_cmds.begin(), visible_cmds.end(),
                            "SET_NETWORK 0 ssid \"VisibleNet\""));

    std::error_code ec;
    std::filesystem::remove_all(cfg_dir, ec);
}

// The monitor connection must still be open when it detaches. libhv and
// wpa_ctrl each close the descriptor they hold, so if they shared one the
// DETACH would go out on an already-closed (or, worse, reused) fd and never
// reach the daemon - and the second close would hit whatever reused it.
TEST_CASE("wpa teardown detaches the monitor connection while it is still open",
          "[network][wpa][1293]") {
    WpaFakeFixture fx;
    auto detaches = [&fx] {
        const std::vector<std::string> cmds = fx.fake->commands();
        return std::count(cmds.begin(), cmds.end(), "DETACH");
    };

    SECTION("stop() with the event loop running") {
        WifiBackendWpaSupplicant backend(kWatchdogMs);
        REQUIRE(backend.start().success());
        REQUIRE(backend.is_running());
        REQUIRE(detaches() == 0);

        backend.stop();
        CHECK(detaches() == 1);
    }

    SECTION("destroying a backend that is still running") {
        {
            WifiBackendWpaSupplicant backend(kWatchdogMs);
            REQUIRE(backend.start().success());
            REQUIRE(backend.is_running());
            REQUIRE(detaches() == 0);
        }
        CHECK(detaches() == 1);
    }
}

namespace {

std::string scan_results(const std::vector<std::string>& rows) {
    std::string raw = "bssid / frequency / signal level / flags / ssid\n";
    for (const auto& row : rows) {
        raw += row + "\n";
    }
    return raw;
}

} // namespace

TEST_CASE("wpa scan results skip a hidden network shown as zero bytes",
          "[network][wpa][hidden_ssid]") {
    WpaFakeFixture fx;
    fx.fake->set_reply("SCAN_RESULTS",
                       scan_results({"aa:bb:cc:dd:ee:01\t2412\t-50\t[WPA2-PSK-CCMP][ESS]\tHome",
                                     "aa:bb:cc:dd:ee:02\t2437\t-60\t[WPA2-PSK-CCMP][ESS]\t"
                                     "\\x00\\x00\\x00\\x00\\x00\\x00"}));
    WifiBackendWpaSupplicant backend(kWatchdogMs);
    REQUIRE(backend.start().success());

    std::vector<WiFiNetwork> networks;
    REQUIRE(backend.get_scan_results(networks).success());
    REQUIRE(networks.size() == 1);
    CHECK(networks[0].ssid == "Home");
}

TEST_CASE("wpa status reads the kernel's address when STATUS carries none",
          "[network][wpa][wifi_status_ip]") {
    WpaFakeFixture fx;
    fx.fake->set_reply("STATUS", "wpa_state=COMPLETED\nssid=Home\nfreq=2412\n");
    WifiBackendWpaSupplicant backend(kWatchdogMs);
    std::vector<std::string> asked;
    backend.set_ipv4_lookup_for_test([&asked](const std::string& netdev) {
        asked.push_back(netdev);
        return std::string("192.168.1.77");
    });
    REQUIRE(backend.start().success());

    WifiBackend::ConnectionStatus status = backend.get_status();
    REQUIRE(status.connected);
    CHECK(status.ip_address == "192.168.1.77");
    REQUIRE_FALSE(asked.empty());
    CHECK(asked.back() == "wlan0");

    SECTION("an address in STATUS is kept and the kernel is not asked") {
        fx.fake->set_reply("STATUS", "wpa_state=COMPLETED\nssid=Home\nip_address=10.0.0.5\n");
        asked.clear();
        status = backend.get_status();
        CHECK(status.ip_address == "10.0.0.5");
        CHECK(asked.empty());
    }

    SECTION("a link that is not associated reports no address") {
        fx.fake->set_reply("STATUS", "wpa_state=SCANNING\n");
        asked.clear();
        status = backend.get_status();
        CHECK_FALSE(status.connected);
        CHECK(status.ip_address.empty());
        CHECK(asked.empty());
    }
}

#endif // !__APPLE__
