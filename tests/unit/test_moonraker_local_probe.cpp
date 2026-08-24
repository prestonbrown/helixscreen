// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_local_probe.cpp
 * @brief Local /proc evidence about Moonraker, for when Moonraker cannot answer.
 *
 * AD5X bundles TAU4PW4H / 865DXBQ7 (v0.99.107): the WebSocket to
 * 127.0.0.1:7125 was refused instantly across two boots. Every Moonraker-derived
 * section of the bundle is fetched THROUGH Moonraker, so all five carried
 * `{"error": "No response from ..."}` and the bundle could not distinguish:
 *
 *   - nothing listening on 7125 at all (service never started, or exited), from
 *   - Moonraker listening on the LAN address only, so a loopback dial is refused
 *     while the printer is perfectly reachable over the network.
 *
 * Those point at completely different fixes. These parsers are what make the
 * next bundle able to tell them apart.
 */

#include "system/moonraker_local_probe.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::diag::candidate_log_paths;
using helix::diag::cmdline_matches_any;
using helix::diag::decode_proc_cmdline;
using helix::diag::parse_listeners_for_port;
using helix::diag::parse_log_hints;
using helix::diag::split_host_port;

namespace {

constexpr const char* HEADER =
    "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout "
    "inode\n";

// 7125 == 0x1BD5. The address column is the host-order read of a __be32, so
// these little-endian-shaped fixtures are what an x86 (or LE MIPS) kernel emits.
std::string tcp4(const std::string& local_hex_addr, const std::string& port_hex,
                 const std::string& state) {
    return std::string(HEADER) + "   0: " + local_hex_addr + ":" + port_hex + " 00000000:0000 " +
           state + " 00000000:00000000 00:00000000 00000000     0        0 1234 1 x\n";
}

} // namespace

TEST_CASE("parse_listeners_for_port finds a loopback-only Moonraker", "[bundle][ad5x]") {
    // 0100007F -> 127.0.0.1, state 0A -> TCP_LISTEN.
    const auto found = parse_listeners_for_port(tcp4("0100007F", "1BD5", "0A"), 7125, false);
    REQUIRE(found.size() == 1);
    CHECK(found[0] == "127.0.0.1:7125");
}

TEST_CASE("parse_listeners_for_port reports the bind address, not just presence",
          "[bundle][ad5x]") {
    // THE DISCRIMINATOR. 6E02A8C0 -> 192.168.2.110: Moonraker IS running, but a
    // dial to 127.0.0.1 is refused. Indistinguishable from "not running" in the
    // bundles that prompted this, and the two need opposite advice.
    SECTION("bound to a single LAN address") {
        const auto found = parse_listeners_for_port(tcp4("6E02A8C0", "1BD5", "0A"), 7125, false);
        REQUIRE(found.size() == 1);
        CHECK(found[0] == "192.168.2.110:7125");
    }

    SECTION("bound to all interfaces") {
        const auto found = parse_listeners_for_port(tcp4("00000000", "1BD5", "0A"), 7125, false);
        REQUIRE(found.size() == 1);
        CHECK(found[0] == "0.0.0.0:7125");
    }
}

TEST_CASE("parse_listeners_for_port ignores non-listening sockets", "[bundle][ad5x]") {
    SECTION("ESTABLISHED (01) on the port is not a listener") {
        CHECK(parse_listeners_for_port(tcp4("0100007F", "1BD5", "01"), 7125, false).empty());
    }

    SECTION("TIME_WAIT (06) is not a listener") {
        CHECK(parse_listeners_for_port(tcp4("0100007F", "1BD5", "06"), 7125, false).empty());
    }

    SECTION("a listener on a different port does not match") {
        // 0x1F90 == 8080.
        CHECK(parse_listeners_for_port(tcp4("0100007F", "1F90", "0A"), 7125, false).empty());
    }

    SECTION("nothing listening yields no addresses — the 'service is down' answer") {
        CHECK(parse_listeners_for_port(HEADER, 7125, false).empty());
    }

    SECTION("a header-only or empty file does not crash or match") {
        CHECK(parse_listeners_for_port("", 7125, false).empty());
        CHECK(parse_listeners_for_port("garbage\n", 7125, false).empty());
    }
}

TEST_CASE("parse_listeners_for_port handles tcp6", "[bundle][ad5x]") {
    const std::string v6_header = HEADER;

    SECTION("wildcard :: is the common dual-stack bind") {
        const std::string body = v6_header +
                                 "   0: 00000000000000000000000000000000:1BD5 "
                                 "00000000000000000000000000000000:0000 0A 00000000:00000000 "
                                 "00:00000000 00000000     0        0 1 1 x\n";
        const auto found = parse_listeners_for_port(body, 7125, true);
        REQUIRE(found.size() == 1);
        CHECK(found[0] == "[::]:7125");
    }

    SECTION("::1 loopback") {
        const std::string body = v6_header +
                                 "   0: 00000000000000000000000001000000:1BD5 "
                                 "00000000000000000000000000000000:0000 0A 00000000:00000000 "
                                 "00:00000000 00000000     0        0 1 1 x\n";
        const auto found = parse_listeners_for_port(body, 7125, true);
        REQUIRE(found.size() == 1);
        CHECK(found[0] == "[::1]:7125");
    }
}

TEST_CASE("split_host_port reads the endpoint off a base URL", "[bundle][ad5x]") {
    std::string host;
    uint16_t port = 0;

    SECTION("the AD5X case") {
        REQUIRE(split_host_port("http://127.0.0.1:7125", host, port));
        CHECK(host == "127.0.0.1");
        CHECK(port == 7125);
    }

    SECTION("no port falls back to the caller's default, not the scheme's") {
        REQUIRE(split_host_port("http://printer.local", host, port, 7125));
        CHECK(host == "printer.local");
        CHECK(port == 80);
    }

    SECTION("a trailing path is not mistaken for a port") {
        REQUIRE(split_host_port("http://192.168.1.5:7125/server/info", host, port));
        CHECK(host == "192.168.1.5");
        CHECK(port == 7125);
    }

    SECTION("bracketed IPv6 keeps its address and takes the port outside") {
        REQUIRE(split_host_port("http://[::1]:7125", host, port));
        CHECK(host == "::1");
        CHECK(port == 7125);
    }

    SECTION("a bare IPv6 literal is not amputated at its last colon") {
        REQUIRE(split_host_port("fd00::1", host, port, 7125));
        CHECK(host == "fd00::1");
        CHECK(port == 7125);
    }

    SECTION("no scheme still parses") {
        REQUIRE(split_host_port("10.0.0.5:7125", host, port));
        CHECK(host == "10.0.0.5");
        CHECK(port == 7125);
    }

    SECTION("empty input fails rather than probing port 7125 of nothing") {
        CHECK_FALSE(split_host_port("", host, port));
        CHECK_FALSE(split_host_port("http://", host, port));
    }
}

TEST_CASE("decode_proc_cmdline collapses the NUL-separated argv", "[bundle][ad5x]") {
    SECTION("NULs become spaces and the trailing one is dropped") {
        const std::string raw("python3\0/opt/moonraker/moonraker.py\0-d\0/root/printer_data\0", 58);
        CHECK(decode_proc_cmdline(raw) ==
              "python3 /opt/moonraker/moonraker.py -d /root/printer_data");
    }

    SECTION("a kernel thread has an empty cmdline") {
        CHECK(decode_proc_cmdline(std::string()).empty());
        CHECK(decode_proc_cmdline(std::string(3, '\0')).empty());
    }
}

TEST_CASE("cmdline_matches_any picks out moonraker and klippy", "[bundle][ad5x]") {
    const std::vector<std::string> needles = {"moonraker", "klippy"};

    CHECK(cmdline_matches_any("python3 /opt/moonraker/moonraker.py -d /data", needles));
    CHECK(cmdline_matches_any("/usr/bin/python3 /opt/klipper/klippy/klippy.py /data/printer.cfg",
                              needles));
    CHECK_FALSE(cmdline_matches_any("/usr/sbin/wpa_supplicant -i wlan0", needles));
    CHECK_FALSE(cmdline_matches_any("", needles));

    SECTION("an empty needle must not match everything") {
        CHECK_FALSE(cmdline_matches_any("anything at all", {""}));
    }
}

/**
 * Deriving the log paths from the daemon's own argv.
 *
 * The alternative was a hardcoded list of per-platform data roots. The reporter
 * runs an AD5X under ZMOD, a layout nobody here has a device to verify, so such a
 * list would be fiction where it mattered and silently empty on every layout not
 * on it. argv is true wherever the daemon is actually running.
 */
TEST_CASE("candidate_log_paths derives log locations from daemon argv", "[bundle][ad5x]") {
    SECTION("-d datapath gives <data>/logs/<name>") {
        std::vector<helix::diag::ProcMatch> procs{
            {101, "python3 /opt/moonraker/moonraker.py -d /root/printer_data"}};
        const auto paths = candidate_log_paths(procs, "moonraker.log");
        REQUIRE_FALSE(paths.empty());
        CHECK(paths[0] == "/root/printer_data/logs/moonraker.log");
    }

    SECTION("-l naming the wanted file is taken verbatim") {
        std::vector<helix::diag::ProcMatch> procs{
            {101, "moonraker -l /mnt/UDISK/printer_data/logs/moonraker.log"}};
        const auto paths = candidate_log_paths(procs, "moonraker.log");
        REQUIRE_FALSE(paths.empty());
        CHECK(paths[0] == "/mnt/UDISK/printer_data/logs/moonraker.log");
    }

    SECTION("one daemon's -l locates the OTHER daemon's log in the same dir") {
        // klippy's -l is often the only argv that names a log directory, and
        // moonraker.log sits beside klippy.log.
        std::vector<helix::diag::ProcMatch> procs{
            {102,
             "python3 /opt/klipper/klippy/klippy.py -l /usr/data/printer_data/logs/klippy.log"}};
        const auto paths = candidate_log_paths(procs, "moonraker.log");
        REQUIRE_FALSE(paths.empty());
        CHECK(paths[0] == "/usr/data/printer_data/logs/moonraker.log");
    }

    SECTION("--datapath=VALUE form is accepted") {
        std::vector<helix::diag::ProcMatch> procs{{101, "moonraker --datapath=/data/pd"}};
        const auto paths = candidate_log_paths(procs, "moonraker.log");
        REQUIRE_FALSE(paths.empty());
        CHECK(paths[0] == "/data/pd/logs/moonraker.log");
    }

    SECTION("-c configfile implies a sibling logs dir") {
        std::vector<helix::diag::ProcMatch> procs{
            {102, "klippy.py -c /home/pi/printer_data/config/printer.cfg"}};
        const auto paths = candidate_log_paths(procs, "klippy.log");
        REQUIRE_FALSE(paths.empty());
        CHECK(paths[0] == "/home/pi/printer_data/logs/klippy.log");
    }

    SECTION("a relative path is rejected — it would resolve against OUR cwd") {
        std::vector<helix::diag::ProcMatch> procs{{101, "moonraker -d printer_data"}};
        CHECK(candidate_log_paths(procs, "moonraker.log").empty());
    }

    SECTION("no usable argv yields nothing rather than a guessed platform root") {
        // THE POINT. An empty result is the honest answer for a layout we cannot
        // see; a fabricated /root/printer_data/... would read as knowledge.
        std::vector<helix::diag::ProcMatch> procs{{101, "moonraker"}};
        CHECK(candidate_log_paths(procs, "moonraker.log").empty());
        CHECK(candidate_log_paths({}, "moonraker.log").empty());
    }

    SECTION("duplicate candidates across daemons are collapsed") {
        std::vector<helix::diag::ProcMatch> procs{
            {101, "moonraker -d /root/printer_data"},
            {102, "klippy.py -l /root/printer_data/logs/klippy.log"}};
        const auto paths = candidate_log_paths(procs, "moonraker.log");
        REQUIRE(paths.size() == 1);
        CHECK(paths[0] == "/root/printer_data/logs/moonraker.log");
    }
}

TEST_CASE("parse_log_hints pulls out only the flags it knows", "[bundle][ad5x]") {
    const auto h =
        parse_log_hints("python3 moonraker.py -d /data/pd -l /data/pd/logs/moonraker.log "
                        "-c /data/pd/config/moonraker.conf");
    CHECK(h.data_path == "/data/pd");
    CHECK(h.log_file == "/data/pd/logs/moonraker.log");
    CHECK(h.config_file == "/data/pd/config/moonraker.conf");

    SECTION("a flag with no value does not consume the next flag") {
        const auto none = parse_log_hints("moonraker -d");
        CHECK(none.data_path.empty());
    }
}

TEST_CASE("listeners_on_port reads the real /proc without false positives", "[bundle][ad5x]") {
    // Exercises the actual /proc read rather than a fixture. The machine this was
    // written on had three ESTABLISHED sockets whose REMOTE port was 7125 (live
    // Moonraker connections to printers on the LAN) and no local listener — the
    // shape that would make a sloppier parser report "Moonraker is listening
    // here" on any developer box or on a screen-only device talking to a remote
    // printer.
    const auto found = helix::diag::listeners_on_port(7125);

    // Machine-state independent: whatever came back must be a well-formed local
    // listener on the port asked for, never a remote endpoint.
    for (const auto& addr : found) {
        CHECK(addr.size() > 5);
        CHECK(addr.rfind(":7125") == addr.size() - 5);
    }

    SECTION("a port nothing can be bound to yields nothing") {
        // Port 0 is never a listening port; a parser matching on the wrong column
        // would still return the many :0000 remote-address entries.
        CHECK(helix::diag::listeners_on_port(0).empty());
    }
}

TEST_CASE("find_moonraker_processes does not report helix-screen itself", "[bundle][ad5x]") {
    // The test binary's own argv can name a moonraker URL, and the app's
    // certainly can. Counting ourselves would turn "Moonraker is not running"
    // into "one moonraker process found", which is the opposite of the finding.
    for (const auto& p : helix::diag::find_moonraker_processes()) {
        CHECK(p.cmdline.find("helix-screen") == std::string::npos);
        CHECK(p.pid > 0);
    }
}
