// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_file_roots.cpp
 * @brief Parsing of server.files.roots, against payloads measured on real printers
 *
 * The file manager's roots are the only thing on the HTTP API that names the
 * writable config directory in absolute terms. Every other signal is relative to
 * a config root we are trying to locate in the first place, which is what made
 * the Creality K2 case unsolvable: Moonraker reports a "moonraker.conf" that the
 * file API cannot serve, and nothing else in the API says where either lives.
 */

#include "moonraker_file_api.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::parse_file_roots;
using helix::writable_root_path;

namespace {

/// Captured from a stock Creality K2 Plus (192.168.30.196) on 2026-08-14.
/// Its moonraker.conf lives in /usr/share/moonraker, i.e. under no root at all.
const char* K2_ROOTS = R"({"result":[
  {"name":"config","path":"/mnt/UDISK/printer_data/config","permissions":"rw"},
  {"name":"logs","path":"/mnt/UDISK/printer_data/logs","permissions":"r"},
  {"name":"gcodes","path":"/mnt/UDISK/printer_data/gcodes","permissions":"rw"},
  {"name":"config_examples","path":"/usr/share/moonraker/config_examples","permissions":"r"},
  {"name":"docs","path":"/usr/share/moonraker/docs","permissions":"r"}
]})";

/// Captured from a stock Creality K1C (192.168.30.182) on 2026-08-14 — the
/// working control, where moonraker.conf IS under the config root.
const char* K1_ROOTS = R"({"result":[
  {"name":"config","path":"/usr/data/printer_data/config","permissions":"rw"},
  {"name":"logs","path":"/usr/data/printer_data/logs","permissions":"r"},
  {"name":"gcodes","path":"/usr/data/printer_data/gcodes","permissions":"rw"}
]})";

} // namespace

TEST_CASE("parse_file_roots reads the K2's config root", "[file_roots]") {
    auto roots = parse_file_roots(json::parse(K2_ROOTS));

    REQUIRE(roots.size() == 5);
    CHECK(roots[0].name == "config");
    CHECK(roots[0].path == "/mnt/UDISK/printer_data/config");
    CHECK(roots[0].permissions == "rw");
    CHECK(roots[0].writable());
}

TEST_CASE("parse_file_roots reads the K1's config root", "[file_roots]") {
    auto roots = parse_file_roots(json::parse(K1_ROOTS));

    REQUIRE(roots.size() == 3);
    CHECK(writable_root_path(roots, "config") == "/usr/data/printer_data/config");
}

TEST_CASE("a root with permissions \"r\" is not writable", "[file_roots]") {
    auto roots = parse_file_roots(json::parse(K2_ROOTS));

    REQUIRE(roots.size() == 5);
    CHECK(roots[1].name == "logs");
    CHECK_FALSE(roots[1].writable());
    CHECK_FALSE(roots[4].writable()); // docs
    // Asking for a read-only root by name yields nothing to write to.
    CHECK(writable_root_path(roots, "logs").empty());
    CHECK(writable_root_path(roots, "docs").empty());
}

TEST_CASE("writable_root_path finds the config root on the K2", "[file_roots]") {
    auto roots = parse_file_roots(json::parse(K2_ROOTS));
    CHECK(writable_root_path(roots, "config") == "/mnt/UDISK/printer_data/config");
    CHECK(writable_root_path(roots, "gcodes") == "/mnt/UDISK/printer_data/gcodes");
}

TEST_CASE("writable_root_path returns empty for a root that is not there", "[file_roots]") {
    auto roots = parse_file_roots(json::parse(K1_ROOTS));
    CHECK(writable_root_path(roots, "timelapse").empty());
    CHECK(writable_root_path(roots, "").empty());
}

TEST_CASE("parse_file_roots accepts a bare result array", "[file_roots]") {
    // JSON-RPC hands the transport the whole envelope, but the HTTP form of the
    // same endpoint is sometimes unwrapped by a caller before it gets here.
    auto roots =
        parse_file_roots(json::parse(R"([{"name":"config","path":"/c","permissions":"rw"}])"));
    REQUIRE(roots.size() == 1);
    CHECK(roots[0].path == "/c");
}

TEST_CASE("parse_file_roots survives a response that is not a root list", "[file_roots]") {
    CHECK(parse_file_roots(json::parse(R"({"result":{"error":"nope"}})")).empty());
    CHECK(parse_file_roots(json::parse("null")).empty());
    CHECK(parse_file_roots(json::parse("7")).empty());
    CHECK(parse_file_roots(json::parse(R"({"no_result":1})")).empty());
}

TEST_CASE("parse_file_roots skips entries with no usable name or path", "[file_roots]") {
    auto roots = parse_file_roots(json::parse(R"({"result":[
      {"name":"config","permissions":"rw"},
      {"path":"/orphan","permissions":"rw"},
      {"name":"gcodes","path":"/g","permissions":"rw"},
      "not-an-object",
      {"name":"logs","path":123,"permissions":"r"}
    ]})"));

    REQUIRE(roots.size() == 1);
    CHECK(roots[0].name == "gcodes");
}

TEST_CASE("a root with no permissions field is not assumed writable", "[file_roots]") {
    // Moonraker always sends it; a fork that does not must not be guessed at, or
    // we would write into a read-only root and report success.
    auto roots = parse_file_roots(json::parse(R"({"result":[{"name":"config","path":"/c"}]})"));
    REQUIRE(roots.size() == 1);
    CHECK_FALSE(roots[0].writable());
    CHECK(writable_root_path(roots, "config").empty());
}

// ============================================================================
// The RPC wiring itself
//
// parse_file_roots() being correct proves nothing if get_file_roots() asks for
// the wrong method — Moonraker answers an unknown method with an error, so the
// failure would look exactly like "this firmware has no roots endpoint" and the
// K2 would quietly fall back to today's broken behaviour.
// ============================================================================

#include "moonraker_client_mock.h"

namespace {

/// Answers one RPC with a canned payload and remembers what was asked.
class RootsClient : public MoonrakerClientMock {
  public:
    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params, std::function<void(const json&)> success_cb,
        std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        last_method = method;
        if (method == "server.files.roots" && success_cb) {
            success_cb(json::parse(K2_ROOTS));
            return 1;
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, success_cb, error_cb, timeout_ms,
                                                 silent, intent);
    }

    std::string last_method;
};

} // namespace

TEST_CASE("get_file_roots asks Moonraker for server.files.roots", "[file_roots]") {
    RootsClient client;
    MoonrakerFileAPI files(client);

    std::vector<FileRoot> got;
    bool errored = false;
    files.get_file_roots([&got](const std::vector<FileRoot>& r) { got = r; },
                         [&errored](const MoonrakerError&) { errored = true; });

    CHECK(client.last_method == "server.files.roots");
    CHECK_FALSE(errored);
    REQUIRE(got.size() == 5);
    CHECK(helix::writable_root_path(got, "config") == "/mnt/UDISK/printer_data/config");
}
