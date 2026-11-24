// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_interface.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace {

/// Build a throwaway directory tree standing in for /sys, /proc and the
/// wpa_supplicant control directory. Real resolution logic, fake filesystem —
/// this is what makes the two-daemon case testable without the device.
struct FakeRoot {
    std::string base;

    FakeRoot() {
        char tmpl[] = "/tmp/helix-wifi-iface-XXXXXX";
        base = ::mkdtemp(tmpl);
    }
    ~FakeRoot() {
        // Recursive delete via nftw would drag in more headers than this needs;
        // the tree is shallow and fully known to us.
        std::string cmd = "rm -rf '" + base + "'";
        (void)::system(cmd.c_str());
    }

    std::string mkdirs(const std::string& rel) const {
        std::string path = base;
        size_t start = 0;
        std::string sub = rel;
        while (start <= sub.size()) {
            size_t slash = sub.find('/', start);
            std::string piece = sub.substr(0, slash == std::string::npos ? sub.size() : slash);
            ::mkdir((base + "/" + piece).c_str(), 0755);
            if (slash == std::string::npos)
                break;
            start = slash + 1;
        }
        return base + "/" + rel;
    }

    void write(const std::string& rel, const std::string& contents) const {
        const size_t slash = rel.find_last_of('/');
        if (slash != std::string::npos)
            mkdirs(rel.substr(0, slash));
        std::ofstream f(base + "/" + rel, std::ios::binary);
        f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    /// /proc/<pid>/cmdline is NUL-separated argv.
    void write_cmdline(int pid, const std::vector<std::string>& argv) const {
        std::string blob;
        for (const auto& a : argv) {
            blob += a;
            blob.push_back('\0');
        }
        write("proc/" + std::to_string(pid) + "/cmdline", blob);
    }
};

helix::wifi::Roots roots_for(const FakeRoot& fr) {
    helix::wifi::Roots r;
    r.sys = fr.base + "/sys";
    r.proc = fr.base + "/proc";
    r.ctrl = fr.base + "/run/wpa_supplicant";
    return r;
}

} // namespace

TEST_CASE("resolve_interface picks the associated socket", "[wifi][interface]") {
    FakeRoot fr;
    fr.mkdirs("run/wpa_supplicant");
    fr.write("run/wpa_supplicant/wlan0", "");
    fr.write("run/wpa_supplicant/wlan1", "");
    fr.mkdirs("sys/class/net/wlan0/wireless");
    fr.mkdirs("sys/class/net/wlan1/wireless");
    fr.write_cmdline(100, {"/usr/sbin/wpa_supplicant", "-i", "wlan0", "-c",
                           "/usr/prog/wifi/wpa_supplicant.conf"});

    // wlan1 answers, but is not associated. wlan0 is.
    auto probe = [](const std::string& sock) -> std::string {
        if (sock.find("wlan0") != std::string::npos)
            return "bssid=aa:bb:cc:dd:ee:ff\nwpa_state=COMPLETED\n";
        return "wpa_state=INACTIVE\n";
    };

    const auto iface = helix::wifi::resolve_interface(roots_for(fr), probe);

    REQUIRE(iface.has_value());
    CHECK(iface->netdev == "wlan0");
    CHECK(iface->conf_path == "/usr/prog/wifi/wpa_supplicant.conf");
    CHECK(iface->daemon_pid == 100);
}

TEST_CASE("resolve_interface reads the conf of the daemon it talks to", "[wifi][interface]") {
    // The AD5X reproduction: two daemons, different -c files. The old
    // first-match /proc walk could return either. Resolution must return the
    // one owning the socket we selected.
    FakeRoot fr;
    fr.mkdirs("run/wpa_supplicant");
    fr.write("run/wpa_supplicant/wlan0", "");
    fr.mkdirs("sys/class/net/wlan0/wireless");
    fr.mkdirs("sys/class/net/wlan1/wireless");
    // Lower PID, iterated first by a naive walk, but NOT our daemon.
    fr.write_cmdline(50, {"/usr/sbin/wpa_supplicant", "-i", "wlan1", "-c", "/etc/ap-mode.conf"});
    fr.write_cmdline(90, {"/usr/sbin/wpa_supplicant", "-i", "wlan0", "-c", "/data/station.conf"});

    auto probe = [](const std::string&) { return std::string("wpa_state=COMPLETED\n"); };
    const auto iface = helix::wifi::resolve_interface(roots_for(fr), probe);

    REQUIRE(iface.has_value());
    CHECK(iface->netdev == "wlan0");
    CHECK(iface->conf_path == "/data/station.conf"); // NOT /etc/ap-mode.conf
    CHECK(iface->daemon_pid == 90);
}

TEST_CASE("resolve_interface falls back when nothing is associated", "[wifi][interface]") {
    FakeRoot fr;
    fr.mkdirs("run/wpa_supplicant");
    fr.write("run/wpa_supplicant/wlan0", "");
    fr.mkdirs("sys/class/net/wlan0/wireless");
    fr.write_cmdline(100, {"/usr/sbin/wpa_supplicant", "-i", "wlan0", "-c", "/etc/wpa.conf"});

    auto probe = [](const std::string&) { return std::string("wpa_state=SCANNING\n"); };
    const auto iface = helix::wifi::resolve_interface(roots_for(fr), probe);

    REQUIRE(iface.has_value());
    CHECK(iface->netdev == "wlan0");
}

TEST_CASE("resolve_interface rejects a socket with no wireless netdev", "[wifi][interface]") {
    FakeRoot fr;
    fr.mkdirs("run/wpa_supplicant");
    fr.write("run/wpa_supplicant/p2p-dev-wlan0", "");
    // No sys/class/net/p2p-dev-wlan0/wireless — not a real station interface.
    auto probe = [](const std::string&) { return std::string("wpa_state=COMPLETED\n"); };

    const auto iface = helix::wifi::resolve_interface(roots_for(fr), probe);
    CHECK_FALSE(iface.has_value());
}

TEST_CASE("resolve_interface returns nullopt when nothing answers", "[wifi][interface]") {
    FakeRoot fr;
    fr.mkdirs("run/wpa_supplicant");
    fr.write("run/wpa_supplicant/wlan0", "");
    fr.mkdirs("sys/class/net/wlan0/wireless");
    auto probe = [](const std::string&) { return std::string(); };

    CHECK_FALSE(helix::wifi::resolve_interface(roots_for(fr), probe).has_value());
}

TEST_CASE("resolve_interface tolerates a missing conf path", "[wifi][interface]") {
    // A daemon launched with -C and no -c. Resolution still succeeds; conf_path
    // is empty and persistence degrades to the HelixScreen-owned store.
    FakeRoot fr;
    fr.mkdirs("run/wpa_supplicant");
    fr.write("run/wpa_supplicant/wlan0", "");
    fr.mkdirs("sys/class/net/wlan0/wireless");
    fr.write_cmdline(100, {"/usr/sbin/wpa_supplicant", "-i", "wlan0", "-C", "/run/wpa_supplicant"});

    auto probe = [](const std::string&) { return std::string("wpa_state=COMPLETED\n"); };
    const auto iface = helix::wifi::resolve_interface(roots_for(fr), probe);

    REQUIRE(iface.has_value());
    CHECK(iface->netdev == "wlan0");
    CHECK(iface->conf_path.empty());
}

TEST_CASE("find_rfkill_node prefers the netdev's own phy link", "[wifi][interface]") {
    FakeRoot fr;
    fr.mkdirs("sys/class/net/wlan0/phy80211/rfkill2");
    fr.mkdirs("sys/class/rfkill/rfkill0");
    fr.write("sys/class/rfkill/rfkill0/type", "bluetooth\n");

    const auto node = helix::wifi::detail::find_rfkill_node(fr.base + "/sys", "wlan0");
    CHECK(node == fr.base + "/sys/class/rfkill/rfkill2");
}

TEST_CASE("find_rfkill_node falls back to a wlan-typed switch", "[wifi][interface]") {
    FakeRoot fr;
    fr.mkdirs("sys/class/net/wlan0/wireless");
    fr.mkdirs("sys/class/rfkill/rfkill0");
    fr.write("sys/class/rfkill/rfkill0/type", "bluetooth\n");
    fr.mkdirs("sys/class/rfkill/rfkill1");
    fr.write("sys/class/rfkill/rfkill1/type", "wlan\n");

    const auto node = helix::wifi::detail::find_rfkill_node(fr.base + "/sys", "wlan0");
    CHECK(node == fr.base + "/sys/class/rfkill/rfkill1");
}

TEST_CASE("find_rfkill_node returns empty when the driver exposes none", "[wifi][interface]") {
    FakeRoot fr;
    fr.mkdirs("sys/class/net/wlan0/wireless");
    CHECK(helix::wifi::detail::find_rfkill_node(fr.base + "/sys", "wlan0").empty());
}

TEST_CASE("has_non_wifi_network_path finds an up eth0", "[wifi][interface]") {
    FakeRoot fr;
    fr.mkdirs("sys/class/net/wlan0/wireless");
    fr.write("sys/class/net/wlan0/operstate", "up\n");
    fr.write("sys/class/net/eth0/operstate", "up\n");

    CHECK(helix::wifi::detail::has_non_wifi_network_path(fr.base + "/sys", "wlan0"));
}

TEST_CASE("has_non_wifi_network_path returns false with only lo", "[wifi][interface]") {
    FakeRoot fr;
    fr.write("sys/class/net/lo/operstate", "unknown\n");

    CHECK_FALSE(helix::wifi::detail::has_non_wifi_network_path(fr.base + "/sys", "wlan0"));
}

TEST_CASE("has_non_wifi_network_path returns false with only the wifi netdev",
          "[wifi][interface]") {
    FakeRoot fr;
    fr.mkdirs("sys/class/net/wlan0/wireless");
    fr.write("sys/class/net/wlan0/operstate", "up\n");
    fr.write("sys/class/net/lo/operstate", "unknown\n");

    CHECK_FALSE(helix::wifi::detail::has_non_wifi_network_path(fr.base + "/sys", "wlan0"));
}

TEST_CASE("has_non_wifi_network_path rejects a second wireless interface as a fallback",
          "[wifi][interface]") {
    FakeRoot fr;
    fr.mkdirs("sys/class/net/wlan0/wireless");
    fr.write("sys/class/net/wlan0/operstate", "up\n");
    // Second radio — up, but still WiFi, so it shares wlan0's failure domain.
    fr.mkdirs("sys/class/net/wlan1/wireless");
    fr.write("sys/class/net/wlan1/operstate", "up\n");

    CHECK_FALSE(helix::wifi::detail::has_non_wifi_network_path(fr.base + "/sys", "wlan0"));
}

TEST_CASE("has_non_wifi_network_path returns false when eth0 is down", "[wifi][interface]") {
    FakeRoot fr;
    fr.mkdirs("sys/class/net/wlan0/wireless");
    fr.write("sys/class/net/wlan0/operstate", "up\n");
    fr.write("sys/class/net/eth0/operstate", "down\n");

    CHECK_FALSE(helix::wifi::detail::has_non_wifi_network_path(fr.base + "/sys", "wlan0"));
}

TEST_CASE("has_non_wifi_network_path excludes a down sit0 tunnel interface", "[wifi][interface]") {
    // sit0 (IPv6-in-IPv4 tunnel) exists on these devices and is DOWN — must
    // not be mistaken for a live fallback path.
    FakeRoot fr;
    fr.mkdirs("sys/class/net/wlan0/wireless");
    fr.write("sys/class/net/wlan0/operstate", "up\n");
    fr.write("sys/class/net/sit0/operstate", "down\n");

    CHECK_FALSE(helix::wifi::detail::has_non_wifi_network_path(fr.base + "/sys", "wlan0"));
}

TEST_CASE("has_non_wifi_network_path returns false when /sys/class/net is missing",
          "[wifi][interface]") {
    FakeRoot fr;
    CHECK_FALSE(helix::wifi::detail::has_non_wifi_network_path(fr.base + "/sys", "wlan0"));
}

TEST_CASE("list_wpa_daemons returns all running daemons with correct data", "[wifi][interface]") {
    // Test that list_wpa_daemons walks the entire /proc tree and returns ALL
    // wpa_supplicant processes, not just the first one.
    FakeRoot fr;
    fr.mkdirs("proc");

    // First daemon: wlan0, with separate -i and -c arguments
    fr.write_cmdline(100, {"/usr/sbin/wpa_supplicant", "-i", "wlan0", "-c", "/data/station.conf"});

    // Second daemon: wlan1, with separate -i and -c arguments
    fr.write_cmdline(200, {"/usr/sbin/wpa_supplicant", "-i", "wlan1", "-c", "/etc/ap-mode.conf"});

    const auto daemons = helix::wifi::detail::list_wpa_daemons(fr.base + "/proc");

    // Should find exactly two daemons
    REQUIRE(daemons.size() == 2);

    // Search for the wlan0 daemon (order not guaranteed from /proc walk)
    auto it0 = std::find_if(daemons.begin(), daemons.end(),
                            [](const auto& d) { return d.iface == "wlan0"; });
    REQUIRE(it0 != daemons.end());
    CHECK(it0->pid == 100);
    CHECK(it0->conf_path == "/data/station.conf");

    // Search for the wlan1 daemon
    auto it1 = std::find_if(daemons.begin(), daemons.end(),
                            [](const auto& d) { return d.iface == "wlan1"; });
    REQUIRE(it1 != daemons.end());
    CHECK(it1->pid == 200);
    CHECK(it1->conf_path == "/etc/ap-mode.conf");
}

TEST_CASE("list_wpa_daemons excludes non-wpa_supplicant processes", "[wifi][interface]") {
    // Non-wpa_supplicant processes (like /usr/bin/klipper) should be filtered out
    FakeRoot fr;
    fr.mkdirs("proc");

    // Valid wpa_supplicant daemon
    fr.write_cmdline(100, {"/usr/sbin/wpa_supplicant", "-i", "wlan0", "-c", "/data/wpa.conf"});

    // Decoy klipper process (should be excluded)
    fr.write_cmdline(150, {"/usr/bin/klipper"});

    // Another decoy process (should be excluded)
    fr.write_cmdline(160, {"/sbin/sh", "-c", "sleep 1000"});

    const auto daemons = helix::wifi::detail::list_wpa_daemons(fr.base + "/proc");

    // Should find only the wpa_supplicant, not the klipper or shell
    REQUIRE(daemons.size() == 1);
    CHECK(daemons[0].pid == 100);
    CHECK(daemons[0].iface == "wlan0");
}

TEST_CASE("list_wpa_daemons handles both separate and joined argument forms", "[wifi][interface]") {
    // Both "-i wlan0" and "-iwlan0" (joined) forms should be parsed correctly
    FakeRoot fr;
    fr.mkdirs("proc");

    // Separate form: -i wlan0
    fr.write_cmdline(100, {"/usr/sbin/wpa_supplicant", "-i", "wlan0", "-c", "/data/conf1.conf"});

    // Joined form: -iwlan1
    fr.write_cmdline(200, {"/usr/sbin/wpa_supplicant", "-iwlan1", "-c/data/conf2.conf"});

    const auto daemons = helix::wifi::detail::list_wpa_daemons(fr.base + "/proc");

    REQUIRE(daemons.size() == 2);

    auto it0 = std::find_if(daemons.begin(), daemons.end(),
                            [](const auto& d) { return d.iface == "wlan0"; });
    REQUIRE(it0 != daemons.end());
    CHECK(it0->conf_path == "/data/conf1.conf");

    auto it1 = std::find_if(daemons.begin(), daemons.end(),
                            [](const auto& d) { return d.iface == "wlan1"; });
    REQUIRE(it1 != daemons.end());
    CHECK(it1->conf_path == "/data/conf2.conf");
}

TEST_CASE("list_wpa_daemons handles missing interface argument", "[wifi][interface]") {
    // A daemon launched without -i should be captured with empty iface
    FakeRoot fr;
    fr.mkdirs("proc");

    fr.write_cmdline(100, {"/usr/sbin/wpa_supplicant", "-c", "/etc/wpa.conf"});

    const auto daemons = helix::wifi::detail::list_wpa_daemons(fr.base + "/proc");

    REQUIRE(daemons.size() == 1);
    CHECK(daemons[0].pid == 100);
    CHECK(daemons[0].iface.empty());
    CHECK(daemons[0].conf_path == "/etc/wpa.conf");
}

TEST_CASE("list_wpa_daemons handles missing config path argument", "[wifi][interface]") {
    // A daemon launched with -i but without -c should be captured with empty conf_path
    FakeRoot fr;
    fr.mkdirs("proc");

    fr.write_cmdline(100, {"/usr/sbin/wpa_supplicant", "-i", "wlan0", "-C", "/run/wpa_supplicant"});

    const auto daemons = helix::wifi::detail::list_wpa_daemons(fr.base + "/proc");

    REQUIRE(daemons.size() == 1);
    CHECK(daemons[0].pid == 100);
    CHECK(daemons[0].iface == "wlan0");
    CHECK(daemons[0].conf_path.empty());
}

TEST_CASE("list_wpa_daemons returns empty vector for missing proc root", "[wifi][interface]") {
    // When proc_root doesn't exist or is unreadable, return empty vector (not error)
    const auto daemons = helix::wifi::detail::list_wpa_daemons("/nonexistent/proc");
    CHECK(daemons.empty());
}

TEST_CASE("list_wpa_daemons returns empty vector for empty proc root", "[wifi][interface]") {
    // When proc_root is an empty string, return empty vector
    const auto daemons = helix::wifi::detail::list_wpa_daemons("");
    CHECK(daemons.empty());
}

TEST_CASE("list_wpa_daemons handles empty proc directory", "[wifi][interface]") {
    // When /proc exists but has no wpa_supplicant processes, return empty vector
    FakeRoot fr;
    fr.mkdirs("proc");

    const auto daemons = helix::wifi::detail::list_wpa_daemons(fr.base + "/proc");
    CHECK(daemons.empty());
}
