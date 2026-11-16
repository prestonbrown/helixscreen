// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_auth.cpp
 * @brief The Moonraker API key reaches the printer it belongs to, over HTTP and
 *        the WebSocket upgrade, and nothing else.
 *
 * Run with: ./build/bin/helix-tests "[moonraker-auth]"
 */

#include "../mocks/mock_websocket_server.h"
#include "../test_helpers/loopback_http_server.h"
#include "config.h"
#include "hv/EventLoopThread.h"
#include "hv/requests.h"
#include "moonraker_auth.h"
#include "moonraker_auth_http.h"
#include "moonraker_client.h"
#include "printer_retarget.h"
#include "utils/network_validation.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::moonraker_auth;

namespace {

/// Leaves no credential behind for the next test.
struct CredentialGuard {
    ~CredentialGuard() {
        set_credential({});
    }
};

} // namespace

TEST_CASE("moonraker auth: the key goes only to its own printer", "[moonraker-auth]") {
    CredentialGuard guard;
    set_credential({"Printer.LAN", 7125, "k"});

    CHECK(key_for("printer.lan", 7125) == "k");
    CHECK(key_for("printer.lan", 7126).empty());
    CHECK(key_for("other.lan", 7125).empty());

    CHECK(key_for_url("ws://printer.lan:7125/websocket") == "k");
    CHECK(key_for_url("http://printer.lan:7125/server/info?x=1") == "k");
    CHECK(key_for_url("http://printer.lan/server/info").empty());
    CHECK(key_for_url("http://printer.lan:80/server/info").empty());

    set_credential({"::1", 7125, "v6"});
    CHECK(key_for_url("ws://[::1]:7125/websocket") == "v6");
    CHECK(key_for_url("ws://[::1]/websocket").empty());

    set_credential({"printer.lan", 7125, ""});
    CHECK(key_for("printer.lan", 7125).empty());
}

TEST_CASE("moonraker auth: what a request does with its header", "[moonraker-auth]") {
    CredentialGuard guard;
    set_credential({"printer.lan", 7125, "k"});

    const HeaderDecision set = header_action("printer.lan", 7125, "");
    CHECK(set.action == HeaderAction::Set);
    CHECK(set.key == "k");
    CHECK(header_action("printer.lan", 7125, "stale").action == HeaderAction::Set);
    CHECK(header_action("printer.lan", 7125, "k").action == HeaderAction::Leave);
    CHECK(header_action("webcam.lan", 80, "").action == HeaderAction::Leave);
    // Another service's key belongs to the caller that set it.
    CHECK(header_action("telemetry.example", 443, "other").action == HeaderAction::Leave);
    // Ours, carried to a host it does not belong to (a redirect), comes off.
    CHECK(header_action("elsewhere.example", 443, "k").action == HeaderAction::Remove);
}

TEST_CASE("moonraker auth: every libhv request to the printer carries the key",
          "[moonraker-auth]") {
    CredentialGuard guard;
    install_http_request_hook();

    std::mutex m;
    std::string seen;
    test::LoopbackHttpServer server;
    server.service().GET("/server/info", [&](HttpRequest* req, HttpResponse* resp) {
        std::lock_guard<std::mutex> lock(m);
        seen = req->GetHeader(kApiKeyHeader);
        return resp->String("{}");
    });
    REQUIRE(server.start());
    const std::string url = server.url("/server/info");
    const int port = std::stoi(url.substr(url.rfind(':') + 1));

    auto get_and_read = [&](const http_headers& headers = {}) {
        {
            std::lock_guard<std::mutex> lock(m);
            seen = "<none>";
        }
        auto resp = requests::get(url.c_str(), headers);
        REQUIRE(resp);
        REQUIRE(resp->status_code == HTTP_STATUS_OK);
        std::lock_guard<std::mutex> lock(m);
        return seen;
    };

    set_credential({"127.0.0.1", port, "secret"});
    CHECK(get_and_read() == "secret");

    // Another port is another server: it gets no key.
    set_credential({"127.0.0.1", port + 1, "secret"});
    CHECK(get_and_read().empty());

    // A key the caller set for that server stays as the caller set it.
    CHECK(get_and_read({{kApiKeyHeader, "callers-own"}}) == "callers-own");
}

TEST_CASE("moonraker auth: the WebSocket upgrade carries the key",
          "[moonraker-auth][moonraker][client]") {
    CredentialGuard guard;
    MockWebSocketServer server;
    REQUIRE(server.start(0) > 0);
    set_credential({"127.0.0.1", server.port(), "ws-secret"});

    auto loop = std::make_shared<hv::EventLoopThread>();
    loop->start();
    auto client = std::make_unique<MoonrakerClient>(loop->loop());
    client->connect(server.url().c_str(), []() {}, []() {});

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && server.connection_count() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(server.connection_count() > 0);
    CHECK(server.last_upgrade_header(kApiKeyHeader) == "ws-secret");

    SECTION("a reconnect to a printer with no key sends none") {
        client->disconnect();
        const auto closed_by = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < closed_by && server.connection_count() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(server.connection_count() == 0);

        set_credential({"127.0.0.1", server.port() + 1, "other-printers"});
        client->connect(server.url().c_str(), []() {}, []() {});
        const auto open_by = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < open_by && server.connection_count() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(server.connection_count() > 0);
        CHECK(server.last_upgrade_header(kApiKeyHeader).empty());
    }

    client->disconnect();
    loop->stop();
    loop->join();
    client.reset();
}

TEST_CASE("moonraker auth: the active printer's saved key", "[moonraker-auth]") {
    Config* config = Config::get_instance();
    const std::string df = config->df();
    nlohmann::json& printer = config->get_json(df.substr(0, df.size() - 1));
    const nlohmann::json saved = printer;

    printer["moonraker_host"] = "printer.lan";
    printer.erase("moonraker_port");

    SECTION("a string key is trimmed, at the address the URL uses") {
        printer["moonraker_api_key"] = "  from-settings  ";
        const Credential c = active_printer_credential();
        CHECK(c.api_key == "from-settings");
        CHECK(c.host == "printer.lan");
        // No saved port: the default the connection URL also falls back to.
        CHECK(c.port == helix::kDefaultMoonrakerPort);
        CHECK(active_printer_ws_url() ==
              "ws://printer.lan:" + std::to_string(helix::kDefaultMoonrakerPort) + "/websocket");
    }
    SECTION("false, absent and blank all mean no key") {
        printer["moonraker_api_key"] = false;
        CHECK(active_printer_credential().api_key.empty());
        printer.erase("moonraker_api_key");
        CHECK(active_printer_credential().api_key.empty());
        printer["moonraker_api_key"] = "   ";
        CHECK(active_printer_credential().api_key.empty());
    }

    printer = saved;
}

TEST_CASE("moonraker auth: a plausible key is 32 hex digits", "[moonraker-auth][api-key]") {
    CHECK(is_plausible_api_key("0123456789abcdef0123456789abcdef"));
    CHECK(is_plausible_api_key("0123456789ABCDEF0123456789ABCDEF"));
    CHECK_FALSE(is_plausible_api_key(""));
    CHECK_FALSE(is_plausible_api_key("0123456789abcdef0123456789abcde"));
    CHECK_FALSE(is_plausible_api_key("0123456789abcdef0123456789abcdef0"));
    CHECK_FALSE(is_plausible_api_key("0123456789abcdef0123456789abcdeg"));
    CHECK_FALSE(is_plausible_api_key(" 123456789abcdef0123456789abcdef"));
}

TEST_CASE("moonraker auth: a refused login is recognized and reported once per credential",
          "[moonraker-auth][api-key]") {
    CredentialGuard guard;
    CHECK(is_unauthorized_error(401, ""));
    CHECK(is_unauthorized_error(0, "Unauthorized"));
    CHECK(is_unauthorized_error(-32000, "unauthorized"));
    CHECK_FALSE(is_unauthorized_error(403, "Forbidden"));
    CHECK_FALSE(is_unauthorized_error(-32601, "Method not found"));

    set_credential({"printer.lan", 7125, ""});
    CHECK_FALSE(unauthorized());
    CHECK(mark_unauthorized());
    CHECK(unauthorized());
    // A reconnect with the same credential is refused the same way; one report covers it.
    CHECK_FALSE(mark_unauthorized());

    // The next connect attempt has not been refused yet.
    set_credential({"printer.lan", 7125, "0123456789abcdef0123456789abcdef"});
    CHECK_FALSE(unauthorized());
    CHECK(mark_unauthorized());
}

TEST_CASE("moonraker auth: the key is set on the active printer's entry only",
          "[moonraker-auth][api-key]") {
    Config* config = Config::get_instance();
    const std::string df = config->df();
    nlohmann::json& printer = config->get_json(df.substr(0, df.size() - 1));
    const nlohmann::json saved = printer;

    set_active_printer_api_key("  0123456789abcdef0123456789abcdef ");
    CHECK(printer["moonraker_api_key"] == "0123456789abcdef0123456789abcdef");
    CHECK(active_printer_credential().api_key == "0123456789abcdef0123456789abcdef");

    set_active_printer_api_key("   ");
    CHECK(printer["moonraker_api_key"] == false);
    CHECK(active_printer_credential().api_key.empty());

    printer = saved;
}
