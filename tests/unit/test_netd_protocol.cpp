// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/netd_protocol.h"
#include "hv/base64.h"
#include "netd_test_server.h"

#include <cstring>
#include <fstream>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::netd::Ack;
using helix::netd::LineAssembler;
using helix::netd::NetdSnapshot;
using helix::netd::ScanRow;
using helix_test::EnvVarGuard;

namespace {

std::string b64(const std::string& raw) {
    return hv::Base64Encode(reinterpret_cast<const unsigned char*>(raw.data()),
                            static_cast<unsigned int>(raw.size()));
}

std::string unb64(const std::string& encoded) {
    return hv::Base64Decode(encoded.c_str(), static_cast<unsigned int>(encoded.size()));
}

} // namespace

// ============================================================================
// parse_snapshot_line — known fields, case-insensitive keys, merge semantics
// ============================================================================

TEST_CASE("netd snapshot line parses every known field", "[netd][protocol]") {
    NetdSnapshot snap;
    REQUIRE(helix::netd::parse_snapshot_line("MODE=WIFI", snap));
    REQUIRE(snap.mode == "WIFI");
    REQUIRE(helix::netd::parse_snapshot_line("STATE=CONNECTING", snap));
    REQUIRE(snap.state == "CONNECTING");
    REQUIRE(helix::netd::parse_snapshot_line("SIGNAL=-47", snap));
    REQUIRE(snap.signal == "-47");
    REQUIRE(helix::netd::parse_snapshot_line("IP=192.168.2.51", snap));
    REQUIRE(snap.ip == "192.168.2.51");
    REQUIRE(helix::netd::parse_snapshot_line("REASON=DHCP_TIMEOUT", snap));
    REQUIRE(snap.reason == "DHCP_TIMEOUT");
    REQUIRE(helix::netd::parse_snapshot_line("PROGRESS=42", snap));
    REQUIRE(snap.progress == "42");
    REQUIRE(helix::netd::parse_snapshot_line("ATTEMPT=3", snap));
    REQUIRE(snap.attempt == "3");
}

TEST_CASE("netd snapshot keys are case-insensitive and split on the FIRST equals",
          "[netd][protocol]") {
    NetdSnapshot snap;
    REQUIRE(helix::netd::parse_snapshot_line("mode=ETHERNET", snap));
    REQUIRE(snap.mode == "ETHERNET");
    REQUIRE(helix::netd::parse_snapshot_line("State=RETRYING", snap));
    REQUIRE(snap.state == "RETRYING");

    // A reason legitimately containing '=' must not lose its tail.
    REQUIRE(helix::netd::parse_snapshot_line("REASON=AUTH_FAILED key=1", snap));
    REQUIRE(snap.reason == "AUTH_FAILED key=1");
}

TEST_CASE("netd snapshot values are whitespace-trimmed", "[netd][protocol]") {
    NetdSnapshot snap;
    REQUIRE(helix::netd::parse_snapshot_line("MODE= OFFLINE \t", snap));
    REQUIRE(snap.mode == "OFFLINE");
}

TEST_CASE("netd snapshot SSID base64 round-trips UTF-8 and decodes invalid base64 to empty",
          "[netd][protocol]") {
    // CJK — multibyte UTF-8 must survive as bytes.
    const std::string cjk = "\xe7\xbd\x91\xe7\xbb\x9c\xe6\x89\x93\xe5\x8d\xb0\xe6\x9c\xba";
    NetdSnapshot snap;
    REQUIRE(helix::netd::parse_snapshot_line("SSID=" + b64(cjk), snap));
    REQUIRE(snap.ssid == cjk);

    // Emoji (4-byte UTF-8 codepoint + variation selector).
    const std::string emoji = "\xf0\x9f\x96\xa8\xef\xb8\x8f caf\xc3\xa9";
    REQUIRE(helix::netd::parse_snapshot_line("SSID=" + b64(emoji), snap));
    REQUIRE(snap.ssid == emoji);

    // Invalid base64 is a known field answered with an empty string, never an error.
    REQUIRE(helix::netd::parse_snapshot_line("SSID=!!not-base64!!", snap));
    REQUIRE(snap.ssid.empty());

    // Empty value round-trips to empty.
    REQUIRE(helix::netd::parse_snapshot_line("SSID=", snap));
    REQUIRE(snap.ssid.empty());
}

TEST_CASE("netd snapshot merges only the field a line carries", "[netd][protocol]") {
    NetdSnapshot snap;
    REQUIRE(helix::netd::parse_snapshot_line("MODE=WIFI", snap));
    REQUIRE(helix::netd::parse_snapshot_line("STATE=CONNECTED", snap));
    REQUIRE(helix::netd::parse_snapshot_line("SSID=" + b64("Studio 5G"), snap));
    REQUIRE(helix::netd::parse_snapshot_line("SIGNAL=-51", snap));
    REQUIRE(helix::netd::parse_snapshot_line("IP=10.0.0.4", snap));
    REQUIRE(helix::netd::parse_snapshot_line("ATTEMPT=1", snap));

    // A later partial update replaces ONLY its own field — everything the
    // daemon did not resend keeps its previous value.
    REQUIRE(helix::netd::parse_snapshot_line("IP=172.16.9.8", snap));
    REQUIRE(snap.ip == "172.16.9.8");
    REQUIRE(snap.mode == "WIFI");
    REQUIRE(snap.state == "CONNECTED");
    REQUIRE(snap.ssid == "Studio 5G");
    REQUIRE(snap.signal == "-51");
    REQUIRE(snap.attempt == "1");

    REQUIRE(helix::netd::parse_snapshot_line("STATE=RETRYING", snap));
    REQUIRE(snap.state == "RETRYING");
    REQUIRE(snap.ip == "172.16.9.8");
}

TEST_CASE("netd unknown lines are not snapshot fields", "[netd][protocol]") {
    NetdSnapshot snap;
    REQUIRE(helix::netd::parse_snapshot_line("MODE=WIFI", snap));

    // Forward compat: a newer daemon may add fields; they must be ignored
    // without disturbing what was already merged.
    REQUIRE_FALSE(helix::netd::parse_snapshot_line("WAT=1", snap));
    REQUIRE_FALSE(helix::netd::parse_snapshot_line("EVENT FOO", snap));
    REQUIRE_FALSE(helix::netd::parse_snapshot_line("OK", snap));
    REQUIRE_FALSE(helix::netd::parse_snapshot_line("ERR BUSY", snap));
    // A scan row is not a snapshot field (dispatch order: ack, scan row, snapshot).
    REQUIRE_FALSE(helix::netd::parse_snapshot_line(
        "FREQUENCY=2437 SIGNAL=-52 SECURITY=WPA-PSK NETWORK=" + b64("x"), snap));
    // No equals sign at all.
    REQUIRE_FALSE(helix::netd::parse_snapshot_line("DISCONNECTED", snap));
    REQUIRE(snap.mode == "WIFI");
}

TEST_CASE("netd connected_state recognises CONNECTED and ONLINE only", "[netd][protocol]") {
    NetdSnapshot snap;
    REQUIRE_FALSE(snap.connected_state());

    snap.state = "CONNECTED";
    REQUIRE(snap.connected_state());

    snap.state = "ONLINE";
    REQUIRE(snap.connected_state());

    snap.state = "CONNECTING";
    REQUIRE_FALSE(snap.connected_state());

    snap.state = "SCANNING";
    REQUIRE_FALSE(snap.connected_state());
}

// ============================================================================
// parse_ack
// ============================================================================

TEST_CASE("netd parse_ack classifies OK and ERR lines", "[netd][protocol]") {
    Ack ack = helix::netd::parse_ack("OK");
    REQUIRE(ack.kind == Ack::Kind::Ok);
    REQUIRE(ack.text.empty());

    ack = helix::netd::parse_ack("OK scan started");
    REQUIRE(ack.kind == Ack::Kind::Ok);
    REQUIRE(ack.text == "scan started");

    ack = helix::netd::parse_ack("ERR WRONG_KEY");
    REQUIRE(ack.kind == Ack::Kind::Err);
    REQUIRE(ack.text == "WRONG_KEY");

    ack = helix::netd::parse_ack("ERR SCAN_IN_PROGRESS");
    REQUIRE(ack.kind == Ack::Kind::Err);
    REQUIRE(ack.text == "SCAN_IN_PROGRESS");
}

TEST_CASE("netd parse_ack returns None for non-ack lines", "[netd][protocol]") {
    for (const char* line : {"OKAY", "OKX", "ERROR", "ERR", "MODE=WIFI", "", "  OK"}) {
        CAPTURE(line);
        REQUIRE(helix::netd::parse_ack(line).kind == Ack::Kind::None);
    }
}

// ============================================================================
// parse_scan_row
// ============================================================================

TEST_CASE("netd scan row parses a full secured saved row", "[netd][protocol]") {
    const std::string line =
        "FREQUENCY=2437 SIGNAL=-52 SECURITY=WPA2-PSK SAVED=1 NETWORK=" + b64("Studio 5G");
    const auto row = helix::netd::parse_scan_row(line);
    REQUIRE(row.has_value());
    REQUIRE(row->ssid == "Studio 5G");
    REQUIRE(row->frequency_mhz == 2437);
    REQUIRE(row->signal_dbm == -52);
    REQUIRE(row->secured);
    REQUIRE(row->security == "WPA2-PSK");
    REQUIRE(row->saved);
}

TEST_CASE("netd scan row open unsaved network is not secured", "[netd][protocol]") {
    const std::string line = "FREQUENCY=5180 SIGNAL=-61 SECURITY=NONE NETWORK=" + b64("Guest");
    const auto row = helix::netd::parse_scan_row(line);
    REQUIRE(row.has_value());
    REQUIRE(row->ssid == "Guest");
    REQUIRE(row->frequency_mhz == 5180);
    REQUIRE(row->signal_dbm == -61);
    REQUIRE_FALSE(row->secured);
    REQUIRE(row->security == "NONE");
    REQUIRE_FALSE(row->saved); // SAVED token absent
}

TEST_CASE("netd scan row tolerates unknown tokens for forward compat", "[netd][protocol]") {
    const std::string line =
        "FREQUENCY=2412 SIGNAL=-40 SECURITY=SAE-PSK BSSID=aa:bb:cc:dd:ee:ff SAVED=0 NETWORK=" +
        b64("\xe7\xbd\x91\xe7\xbb\x9c");
    const auto row = helix::netd::parse_scan_row(line);
    REQUIRE(row.has_value());
    REQUIRE(row->ssid == "\xe7\xbd\x91\xe7\xbb\x9c");
    REQUIRE(row->secured);     // "SAE-PSK" contains PSK
    REQUIRE_FALSE(row->saved); // SAVED=0
}

TEST_CASE("netd scan row rejects malformed rows", "[netd][protocol]") {
    // Missing the NETWORK= marker entirely.
    REQUIRE_FALSE(
        helix::netd::parse_scan_row("FREQUENCY=2437 SIGNAL=-52 SECURITY=WPA-PSK").has_value());
    // Non-integer FREQUENCY.
    REQUIRE_FALSE(
        helix::netd::parse_scan_row("FREQUENCY=2.4G SIGNAL=-52 NETWORK=" + b64("x")).has_value());
    // Empty FREQUENCY.
    REQUIRE_FALSE(
        helix::netd::parse_scan_row("FREQUENCY= SIGNAL=-52 NETWORK=" + b64("x")).has_value());
    // Non-integer SIGNAL.
    REQUIRE_FALSE(
        helix::netd::parse_scan_row("FREQUENCY=2437 SIGNAL=weak NETWORK=" + b64("x")).has_value());
    // Missing SIGNAL token entirely.
    REQUIRE_FALSE(helix::netd::parse_scan_row("FREQUENCY=2437 SECURITY=WPA-PSK NETWORK=" + b64("x"))
                      .has_value());
    // Empty decoded SSID (empty blob).
    REQUIRE_FALSE(helix::netd::parse_scan_row("FREQUENCY=2437 SIGNAL=-52 NETWORK=").has_value());
    // Empty decoded SSID (blob decodes to nothing — invalid base64).
    REQUIRE_FALSE(helix::netd::parse_scan_row("FREQUENCY=2437 SIGNAL=-52 NETWORK=###").has_value());
    // Does not start with FREQUENCY= (leading space, other key first).
    REQUIRE_FALSE(
        helix::netd::parse_scan_row(" SIGNAL=-52 FREQUENCY=2437 NETWORK=" + b64("x")).has_value());
    // Marker without the required leading single space ("NETWORK=" glued to a token).
    REQUIRE_FALSE(
        helix::netd::parse_scan_row("FREQUENCY=2437 SIGNAL=-52 SECURITY=NONE-NETWORK=" + b64("x"))
            .has_value());
}

// ============================================================================
// Command encoders
// ============================================================================

TEST_CASE("netd plain commands encode without a trailing newline", "[netd][protocol]") {
    REQUIRE(helix::netd::encode_get() == "GET");
    REQUIRE(helix::netd::encode_subscribe() == "SUBSCRIBE");
    REQUIRE(helix::netd::encode_scan() == "SCAN");
    REQUIRE(helix::netd::encode_cancel() == "CANCEL");

    for (const std::string& cmd : {helix::netd::encode_get(), helix::netd::encode_subscribe(),
                                   helix::netd::encode_scan(), helix::netd::encode_cancel()}) {
        REQUIRE(cmd.back() != '\n');
        REQUIRE(cmd.find(' ') == std::string::npos);
    }
}

TEST_CASE("netd encode_connect_wifi is byte-exact with base64 ssid and psk", "[netd][protocol]") {
    const std::string encoded = helix::netd::encode_connect_wifi("Studio 5G", "hunter2");
    REQUIRE(encoded == "CONNECT_WIFI ssid=" + b64("Studio 5G") + " psk=" + b64("hunter2"));
    REQUIRE(encoded.back() != '\n');

    // Independently decode both tokens back to the originals.
    const size_t ssid_at = encoded.find("ssid=") + 5;
    const size_t psk_at = encoded.find(" psk=") + 5;
    REQUIRE(unb64(encoded.substr(ssid_at, psk_at - 5 - ssid_at)) == "Studio 5G");
    REQUIRE(unb64(encoded.substr(psk_at)) == "hunter2");
}

TEST_CASE("netd encode_connect_wifi base64 round-trips UTF-8 parameters", "[netd][protocol]") {
    const std::string ssid = "\xe7\xbd\x91\xe7\xbb\x9c\xf0\x9f\x96\xa8\xef\xb8\x8f";
    const std::string psk = "p\xc3\xa4ssw\xc3\xb6rd!";
    const std::string encoded = helix::netd::encode_connect_wifi(ssid, psk);
    REQUIRE(encoded == "CONNECT_WIFI ssid=" + b64(ssid) + " psk=" + b64(psk));
    REQUIRE(unb64(encoded.substr(encoded.find("ssid=") + 5,
                                 encoded.find(" psk=") - encoded.find("ssid=") - 5)) == ssid);
}

TEST_CASE("netd encode_connect_wifi omits the psk token for open networks", "[netd][protocol]") {
    const std::string encoded = helix::netd::encode_connect_wifi("CoffeeShop", "");
    REQUIRE(encoded == "CONNECT_WIFI ssid=" + b64("CoffeeShop"));
    // Omitted ENTIRELY: no "psk=" anywhere and no trailing space before the newline
    // the caller appends.
    REQUIRE(encoded.find("psk=") == std::string::npos);
    REQUIRE(encoded.back() != ' ');
}

TEST_CASE("netd encode_connect_wifi empty ssid encodes an empty token", "[netd][protocol]") {
    // An empty ssid is a protocol error the daemon reports (MISSING_SSID);
    // the encoder must still produce a well-formed line, not garbage padding.
    REQUIRE(helix::netd::encode_connect_wifi("", "pw") == "CONNECT_WIFI ssid= psk=" + b64("pw"));
}

// ============================================================================
// LineAssembler
// ============================================================================

TEST_CASE("netd LineAssembler carries a partial line across feeds", "[netd][protocol]") {
    LineAssembler assembler;
    const auto first = assembler.feed("STATE=CONN");
    REQUIRE(first.empty());

    const auto second = assembler.feed("ECTED\nSIGNAL=-4");
    REQUIRE(second.size() == 1);
    REQUIRE(second[0] == "STATE=CONNECTED");

    // "-4" is still partial — nothing to return until its newline arrives.
    REQUIRE(assembler.feed("").empty());

    const auto third = assembler.feed("7\n");
    REQUIRE(third.size() == 1);
    REQUIRE(third[0] == "SIGNAL=-47");
}

TEST_CASE("netd LineAssembler emits several lines from one chunk", "[netd][protocol]") {
    LineAssembler assembler;
    const auto lines = assembler.feed("MODE=WIFI\nIP=1.2.3.4\nOK\n");
    REQUIRE(lines.size() == 3);
    REQUIRE(lines[0] == "MODE=WIFI");
    REQUIRE(lines[1] == "IP=1.2.3.4");
    REQUIRE(lines[2] == "OK");
}

TEST_CASE("netd LineAssembler strips CR and skips empty lines", "[netd][protocol]") {
    LineAssembler assembler;
    const auto lines = assembler.feed("MODE=OFFLINE\r\n\r\nOK\r\n\nERR BUSY\n");
    REQUIRE(lines.size() == 3);
    REQUIRE(lines[0] == "MODE=OFFLINE");
    REQUIRE(lines[1] == "OK");
    REQUIRE(lines[2] == "ERR BUSY");
}

TEST_CASE("netd LineAssembler keeps trailing bytes without a newline until more arrive",
          "[netd][protocol]") {
    LineAssembler assembler;
    const auto lines = assembler.feed("OK\nPARTIAL_NO_NEWLINE");
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0] == "OK");

    REQUIRE(assembler.feed("_STILL").empty());
    const auto done = assembler.feed("_DONE\n");
    REQUIRE(done.size() == 1);
    REQUIRE(done[0] == "PARTIAL_NO_NEWLINE_STILL_DONE");
}

TEST_CASE("netd LineAssembler discards an oversized partial and recovers", "[netd][protocol]") {
    // 256 KiB = 262144 bytes. A partial strictly larger than that is garbage
    // (nothing legitimate frames a >256KiB line); it is dropped, not hoarded.
    LineAssembler assembler;

    SECTION("just at the limit survives") {
        const std::string at_limit(262144, 'x');
        REQUIRE(assembler.feed(at_limit).empty());
        const auto lines = assembler.feed("\n");
        REQUIRE(lines.size() == 1);
        REQUIRE(lines[0].size() == 262144);
    }

    SECTION("over the limit is discarded and the next line still parses") {
        const std::string over_limit(262145, 'x');
        REQUIRE(assembler.feed(over_limit).empty());
        const auto lines = assembler.feed("OK\n");
        REQUIRE(lines.size() == 1);
        REQUIRE(lines[0] == "OK");
    }
}

TEST_CASE("netd LineAssembler reset drops the carried partial", "[netd][protocol]") {
    LineAssembler assembler;
    REQUIRE(assembler.feed("STATE=CONN").empty());
    assembler.reset();
    const auto lines = assembler.feed("ECTED\n");
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0] == "ECTED");
}

// ============================================================================
// socket_path / binary_path / available — per-call getenv, filesystem probe
// ============================================================================

TEST_CASE("netd socket_path and binary_path read the env per call and default",
          "[netd][protocol]") {
    EnvVarGuard sock("HELIX_NETD_SOCKET");
    EnvVarGuard bin("HELIX_NETD_BIN");

    sock.unset();
    bin.unset();
    // Defaults may not exist on this machine, but the STRINGS are fixed.
    // (Only this module may spell these two literals.)
    REQUIRE(helix::netd::socket_path() == "/run/netd.sock");
    REQUIRE(helix::netd::binary_path() == "/opt/config/mod/.bin/exec/netd");

    sock.set("/tmp/one.sock");
    REQUIRE(helix::netd::socket_path() == "/tmp/one.sock");
    sock.set("/tmp/two.sock");
    REQUIRE(helix::netd::socket_path() == "/tmp/two.sock");

    bin.set("/tmp/netd-alt");
    REQUIRE(helix::netd::binary_path() == "/tmp/netd-alt");

    // Empty string falls back to the default, not to "".
    sock.set("");
    REQUIRE(helix::netd::socket_path() == "/run/netd.sock");
}

TEST_CASE("netd available probes socket stat and binary X_OK per call", "[netd][protocol]") {
    EnvVarGuard sock("HELIX_NETD_SOCKET");
    EnvVarGuard bin("HELIX_NETD_BIN");

    char dir_template[] = "/tmp/helix_netd_probe_XXXXXX";
    char* dir = ::mkdtemp(dir_template);
    REQUIRE(dir != nullptr);
    const std::string dir_s(dir);

    // A REAL bound + listening stream socket.
    const std::string sock_path = dir_s + "/netd.sock";
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(::listen(fd, 1) == 0);

    // An executable regular file (0700: owner-executable, nothing else).
    const std::string bin_path = dir_s + "/netd";
    {
        std::ofstream out(bin_path, std::ios::binary);
        out << "#!/bin/sh\n";
    }
    REQUIRE(::chmod(bin_path.c_str(), 0700) == 0);

    // A regular file WITHOUT any execute bit.
    const std::string noexec_path = dir_s + "/netd-noexec";
    {
        std::ofstream out(noexec_path, std::ios::binary);
        out << "not executable\n";
    }
    REQUIRE(::chmod(noexec_path.c_str(), 0600) == 0);

    SECTION("real bound socket => true (binary default absent)") {
        bin.unset();
        sock.set(sock_path);
        REQUIRE(helix::netd::available());
    }

    SECTION("nonexistent socket path and default binary => false") {
        bin.unset();
        sock.set(dir_s + "/no-such.sock");
        REQUIRE_FALSE(helix::netd::available());
    }

    SECTION("executable binary overrides dead socket => true") {
        sock.set(dir_s + "/no-such.sock");
        bin.set(bin_path);
        REQUIRE(helix::netd::available());
    }

    SECTION("binary present but not executable => false") {
        sock.set(dir_s + "/no-such.sock");
        bin.set(noexec_path);
        REQUIRE_FALSE(helix::netd::available());
    }

    SECTION("both unset with nonexistent defaults => false") {
        sock.unset();
        bin.unset();
        REQUIRE_FALSE(helix::netd::available());
    }

    SECTION("empty-string env values fall back to the defaults") {
        sock.set("");
        bin.set("");
        REQUIRE_FALSE(helix::netd::available());
    }

    ::close(fd);
    ::unlink(sock_path.c_str());
    ::unlink(bin_path.c_str());
    ::unlink(noexec_path.c_str());
    ::rmdir(dir_s.c_str());
}

// ============================================================================
// query_snapshot — blocking one-shot GET against a fake daemon
// ============================================================================

namespace {

// Accepts one connection on `path`, reads a line, replies with the canned
// bytes, closes. Records the line the client sent so the test can assert the
// wire request was exactly "GET".
//
// Both the accept and the read are bounded: a client that never connects or
// never speaks (a stubbed or mutated query_snapshot during red/mutation runs)
// must not wedge the suite at worker.join().
struct FakeNetd {
    static constexpr int kWaitMs = 2000;

    std::string path;
    std::string reply;
    bool silent = false; // accept + read, then close without writing
    std::string received_line;
    int listen_fd = -1;

    static bool wait_readable(int fd, int timeout_ms) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        return ::poll(&pfd, 1, timeout_ms) > 0;
    }

    bool start() {
        listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd < 0)
            return false;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path))
            return false;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            return false;
        if (::listen(listen_fd, 1) != 0)
            return false;
        return true;
    }

    void serve() {
        if (!wait_readable(listen_fd, kWaitMs))
            return;
        int cfd = ::accept(listen_fd, nullptr, nullptr);
        if (cfd < 0)
            return;
        std::string buf;
        if (wait_readable(cfd, kWaitMs)) {
            char chunk[256];
            const ssize_t n = ::recv(cfd, chunk, sizeof(chunk), 0);
            if (n > 0)
                buf.append(chunk, static_cast<size_t>(n));
        }
        const size_t nl = buf.find('\n');
        received_line = nl == std::string::npos ? buf : buf.substr(0, nl);
        if (!silent && !reply.empty()) {
            // Two writes: the client must cope with the reply arriving in
            // more than one read. (Storing the results, not asserting here:
            // Catch2 assertions must stay on the main thread.)
            const size_t half = reply.size() / 2;
            [[maybe_unused]] const ssize_t n1 = ::write(cfd, reply.data(), half);
            [[maybe_unused]] const ssize_t n2 =
                ::write(cfd, reply.data() + half, reply.size() - half);
        }
        ::close(cfd);
    }

    void teardown() {
        if (listen_fd >= 0)
            ::close(listen_fd);
        ::unlink(path.c_str());
    }
};

} // namespace

TEST_CASE("netd query_snapshot reaches a live daemon and parses the snapshot", "[netd][protocol]") {
    char dir_template[] = "/tmp/helix_netd_query_XXXXXX";
    char* dir = ::mkdtemp(dir_template);
    REQUIRE(dir != nullptr);
    const std::string dir_s(dir);
    EnvVarGuard sock("HELIX_NETD_SOCKET");
    EnvVarGuard bin("HELIX_NETD_BIN");

    const std::string ssid = "Studio 5G";
    FakeNetd server;
    server.path = dir_s + "/netd.sock";
    server.reply = "MODE=WIFI\nSTATE=ONLINE\nSSID=" + b64(ssid) +
                   "\nSIGNAL=-47\nIP=192.168.2.51\nREASON=DONE\nPROGRESS=100\nATTEMPT=2\nOK\n";
    REQUIRE(server.start());

    std::thread worker([&server] { server.serve(); });

    sock.set(server.path);
    bin.unset();
    const helix::netd::QueryResult result = helix::netd::query_snapshot(1000);
    worker.join();
    server.teardown();

    REQUIRE(result.reached);
    REQUIRE(result.snapshot.mode == "WIFI");
    REQUIRE(result.snapshot.state == "ONLINE");
    REQUIRE(result.snapshot.ssid == ssid);
    REQUIRE(result.snapshot.signal == "-47");
    REQUIRE(result.snapshot.ip == "192.168.2.51");
    REQUIRE(result.snapshot.reason == "DONE");
    REQUIRE(result.snapshot.progress == "100");
    REQUIRE(result.snapshot.attempt == "2");
    REQUIRE(result.snapshot.connected_state());
    REQUIRE(server.received_line == "GET");

    ::rmdir(dir_s.c_str());
}

TEST_CASE("netd query_snapshot reports unreachable on a nonexistent socket path",
          "[netd][protocol]") {
    EnvVarGuard sock("HELIX_NETD_SOCKET");
    EnvVarGuard bin("HELIX_NETD_BIN");
    sock.set("/tmp/helix_netd_no_such_dir/netd.sock");
    bin.unset();

    const helix::netd::QueryResult result = helix::netd::query_snapshot(200);
    REQUIRE_FALSE(result.reached);
    REQUIRE(result.snapshot.mode.empty());
}

TEST_CASE("netd query_snapshot reports unreachable when the socket file exists but nobody listens",
          "[netd][protocol]") {
    char dir_template[] = "/tmp/helix_netd_dead_XXXXXX";
    char* dir = ::mkdtemp(dir_template);
    REQUIRE(dir != nullptr);
    const std::string dir_s(dir);
    EnvVarGuard sock("HELIX_NETD_SOCKET");
    EnvVarGuard bin("HELIX_NETD_BIN");

    // Bind then close WITHOUT unlinking: the socket file remains, connect()
    // is refused — the "netd died but its socket file lingers" case.
    const std::string stale = dir_s + "/stale.sock";
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, stale.c_str(), sizeof(addr.sun_path) - 1);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    ::close(fd);

    sock.set(stale);
    bin.unset();
    const helix::netd::QueryResult result = helix::netd::query_snapshot(200);
    REQUIRE_FALSE(result.reached);

    ::unlink(stale.c_str());
    ::rmdir(dir_s.c_str());
}

TEST_CASE("netd query_snapshot reports unreachable when the daemon answers nothing",
          "[netd][protocol]") {
    char dir_template[] = "/tmp/helix_netd_silent_XXXXXX";
    char* dir = ::mkdtemp(dir_template);
    REQUIRE(dir != nullptr);
    const std::string dir_s(dir);
    EnvVarGuard sock("HELIX_NETD_SOCKET");
    EnvVarGuard bin("HELIX_NETD_BIN");

    FakeNetd server;
    server.path = dir_s + "/netd.sock";
    server.silent = true;
    REQUIRE(server.start());
    std::thread worker([&server] { server.serve(); });

    sock.set(server.path);
    bin.unset();
    const helix::netd::QueryResult result = helix::netd::query_snapshot(150);
    worker.join();
    server.teardown();

    REQUIRE_FALSE(result.reached);
    ::rmdir(dir_s.c_str());
}
