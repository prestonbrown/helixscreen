// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ipp_printer_http.cpp
 * @brief The IPP job POST against a real loopback socket server.
 *
 * Some printers (Canon PIXMA) answer a POST with an unsolicited interim
 * `HTTP/1.1 100 Continue` before the final response, with or without an
 * `Expect:` header. The client has to read past it to the final response
 * (RFC 9110 15.2). The server here is a raw socket so the test controls
 * exactly which bytes arrive in which write (prestonbrown/helixscreen#1822).
 */

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/test_cert.h"
#include "config.h"
#include "hv/HttpMessage.h"
#include "ipp_print_modal.h"
#include "ipp_printer.h"
#include "ipp_protocol.h"
#include "label_printer_settings.h"
#include "label_printer_utils.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr const char* kContinue = "HTTP/1.1 100 Continue\r\n\r\n";

/// A minimal IPP response body: version 2.0, successful-ok, request-id 1, end tag.
std::string ipp_ok_body() {
    const unsigned char bytes[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03};
    return std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

std::string final_ok_response() {
    const auto body = ipp_ok_body();
    return "HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

/// One scripted connection: whether it speaks TLS, and the chunks written back once the
/// whole request has arrived.
struct ScriptedConnection {
    bool tls = false;
    std::vector<std::string> chunks;
};

/// HTTP server on 127.0.0.1 that accepts one connection per script, in order, reads each
/// whole request and then writes that script's chunks, pausing between chunks so each
/// lands in its own recv. Once every script has run it stops listening, so a further
/// connect is refused instead of hanging.
class ScriptedServer {
  public:
    explicit ScriptedServer(std::vector<std::string> chunks, int gap_ms = 150)
        : ScriptedServer({ScriptedConnection{false, std::move(chunks)}}, nullptr, gap_ms) {}

    /// `tls_ctx` serves every connection whose script sets `tls`; the server does not own it.
    ScriptedServer(std::vector<ScriptedConnection> connections, SSL_CTX* tls_ctx, int gap_ms = 150)
        : connections_(std::move(connections)), tls_ctx_(tls_ctx), gap_ms_(gap_ms) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(fd, 4);
        socklen_t len = sizeof(addr);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        listen_fd_ = fd;
        thread_ = std::thread([this] { serve(); });
    }

    ~ScriptedServer() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        stop_cv_.notify_all();
        close_listener();
        if (thread_.joinable())
            thread_.join();
    }

    uint16_t port() const {
        return port_;
    }
    size_t body_bytes_received() const {
        return body_bytes_.load();
    }
    /// The last request line, e.g. "POST /ipp/print HTTP/1.1"; empty until a request arrived.
    std::string request_line() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.empty() ? std::string() : requests_.back().line;
    }
    /// The last request body as received; empty until a request arrived.
    std::string body() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.empty() ? std::string() : requests_.back().body;
    }
    /// The body of request `i`, in arrival order; empty when fewer arrived.
    std::string body(size_t i) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return i < requests_.size() ? requests_[i].body : std::string();
    }
    /// The line of request `i`, in arrival order; empty when fewer arrived.
    std::string request_line(size_t i) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return i < requests_.size() ? requests_[i].line : std::string();
    }
    /// How many requests arrived whole.
    size_t request_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.size();
    }

  private:
    struct Request {
        std::string line;
        std::string body;
    };

    void close_listener() {
        const int fd = listen_fd_.exchange(-1);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
    }

    void serve() {
        for (const auto& script : connections_) {
            const int lfd = listen_fd_.load();
            if (lfd < 0)
                return;
            const int fd = ::accept(lfd, nullptr, nullptr);
            if (fd < 0)
                return;
            serve_one(fd, script);
        }
        close_listener();
    }

    void serve_one(int fd, const ScriptedConnection& script) {
        timeval tv{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        SSL* ssl = nullptr;
        if (script.tls) {
            ssl = SSL_new(tls_ctx_);
            SSL_set_fd(ssl, fd);
            if (SSL_accept(ssl) != 1) {
                SSL_free(ssl);
                ::close(fd);
                return;
            }
        }
        auto read_some = [ssl, fd](char* buf, size_t n) -> long {
            return ssl ? SSL_read(ssl, buf, static_cast<int>(n)) : ::recv(fd, buf, n, 0);
        };
        auto write_all = [ssl, fd](const std::string& out) {
            if (ssl)
                SSL_write(ssl, out.data(), static_cast<int>(out.size()));
            else
                ::send(fd, out.data(), out.size(), MSG_NOSIGNAL);
        };

        std::string request;
        char buf[4096];
        size_t header_end = std::string::npos;
        size_t content_length = 0;
        while (true) {
            const long n = read_some(buf, sizeof(buf));
            if (n <= 0)
                break;
            request.append(buf, static_cast<size_t>(n));
            if (header_end == std::string::npos) {
                header_end = request.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    auto pos = request.find("Content-Length: ");
                    if (pos != std::string::npos && pos < header_end)
                        content_length = std::stoul(request.substr(pos + 16));
                }
            }
            if (header_end != std::string::npos &&
                request.size() >= header_end + 4 + content_length)
                break;
        }
        if (header_end != std::string::npos) {
            body_bytes_ = request.size() - header_end - 4;
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(
                {request.substr(0, request.find("\r\n")), request.substr(header_end + 4)});
        }

        // The rest of a script the client gave up on is dropped once the test ends.
        for (size_t i = 0; i < script.chunks.size(); ++i) {
            if (i > 0) {
                std::unique_lock<std::mutex> lock(mutex_);
                if (stop_cv_.wait_for(lock, std::chrono::milliseconds(gap_ms_),
                                      [this] { return stopping_; }))
                    break;
            }
            write_all(script.chunks[i]);
        }
        // Keep the connection open until the client hangs up, as a printer would.
        while (read_some(buf, sizeof(buf)) > 0) {
        }
        SSL_free(ssl);
        ::close(fd);
    }

    std::vector<ScriptedConnection> connections_;
    SSL_CTX* tls_ctx_ = nullptr;
    int gap_ms_;
    mutable std::mutex mutex_;
    std::condition_variable stop_cv_;
    bool stopping_ = false;
    std::vector<Request> requests_;
    std::atomic<int> listen_fd_{-1};
    uint16_t port_ = 0;
    std::atomic<size_t> body_bytes_{0};
    std::thread thread_;
};

std::vector<uint8_t> job_body() {
    return std::vector<uint8_t>(20000, 0x5A);
}

void require_ipp_ok(const std::shared_ptr<HttpResponse>& resp) {
    REQUIRE(resp != nullptr);
    REQUIRE(static_cast<int>(resp->status_code) == 200);
    auto ipp_resp =
        ipp::parse_response(reinterpret_cast<const uint8_t*>(resp->body.data()), resp->body.size());
    REQUIRE(ipp_resp.is_success());
}

} // namespace

TEST_CASE("IPP POST reads past an interim 100 Continue to the final response",
          "[label][ipp][ipp_http]") {
    ScriptedServer server({kContinue, final_ok_response()});
    auto body = job_body();

    auto resp = ipp::post_request("127.0.0.1", server.port(), "ipp/print", body, 10);

    require_ipp_ok(resp);
    REQUIRE(server.body_bytes_received() == body.size());
}

TEST_CASE("IPP POST handles 100 Continue and the final response in one write",
          "[label][ipp][ipp_http]") {
    ScriptedServer server({std::string(kContinue) + final_ok_response()});

    auto resp = ipp::post_request("127.0.0.1", server.port(), "ipp/print", job_body(), 10);

    require_ipp_ok(resp);
}

TEST_CASE("IPP POST reads past several interim responses", "[label][ipp][ipp_http]") {
    ScriptedServer server({kContinue, "HTTP/1.1 102 Processing\r\n\r\n", final_ok_response()});

    auto resp = ipp::post_request("127.0.0.1", server.port(), "ipp/print", job_body(), 10);

    require_ipp_ok(resp);
}

TEST_CASE("IPP POST returns a final response that has no interim", "[label][ipp][ipp_http]") {
    ScriptedServer server({final_ok_response()});

    auto resp = ipp::post_request("127.0.0.1", server.port(), "ipp/print", job_body(), 10);

    require_ipp_ok(resp);
}

TEST_CASE("IPP POST returns nullptr when only an interim response arrives",
          "[label][ipp][ipp_http]") {
    ScriptedServer server({kContinue});

    auto resp = ipp::post_request("127.0.0.1", server.port(), "ipp/print", job_body(), 2);

    REQUIRE(resp == nullptr);
}

TEST_CASE("IPP POST gives up at its deadline while interim responses keep coming",
          "[label][ipp][ipp_http]") {
    // Each 100 lands well inside the per-read timeout, so only an overall deadline
    // stops the wait.
    ScriptedServer server(std::vector<std::string>(12, kContinue), 400);

    const auto start = std::chrono::steady_clock::now();
    auto resp = ipp::post_request("127.0.0.1", server.port(), "ipp/print", job_body(), 1);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE(resp == nullptr);
    REQUIRE(elapsed < std::chrono::milliseconds(2500));
}

namespace {

/// Points the label-printer settings at a loopback IPP server for one test.
struct IppSettingsScope {
    explicit IppSettingsScope(uint16_t port, const std::string& rp) {
        Config::get_instance();
        auto& s = LabelPrinterSettingsManager::instance();
        s.init_subjects();
        s.set_printer_type("network");
        s.set_printer_protocol("ipp");
        s.set_printer_address("127.0.0.1");
        s.set_printer_port(port);
        s.set_ipp_resource_path(rp);
        s.set_label_size_index(0);
    }
    ~IppSettingsScope() {
        auto& s = LabelPrinterSettingsManager::instance();
        s.set_printer_address("");
        s.set_ipp_resource_path("");
        s.set_printer_port(9100);
        s.set_printer_protocol("raw");
        s.deinit_subjects();
    }
};

SpoolInfo test_spool() {
    SpoolInfo spool;
    spool.id = 42;
    spool.vendor = "Hatchbox";
    spool.material = "PLA";
    spool.color_hex = "#1A1A2E";
    return spool;
}

class OkableIppPrintModal : public IppPrintModal {
  public:
    using IppPrintModal::on_ok;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "print_spool_label posts to the configured IPP resource path",
                 "[label][ipp_http][ipp_rp]") {
    ScriptedServer server({final_ok_response()});
    IppSettingsScope scope(server.port(), "/printers/Canon_TS8350");

    bool done = false;
    bool ok = false;
    print_spool_label(test_spool(), [&](bool success, const std::string&) {
        ok = success;
        done = true;
    });

    REQUIRE(wait_until([&] { return done; }, 15000));
    REQUIRE(server.request_line() == "POST /printers/Canon_TS8350 HTTP/1.1");
    REQUIRE(ok);
}

TEST_CASE_METHOD(LVGLUITestFixture, "The IPP print modal posts to the configured resource path",
                 "[label][ipp_http][ipp_rp]") {
    ScriptedServer server({final_ok_response()});
    IppSettingsScope scope(server.port(), "printers/Canon_TS8350");

    bool done = false;
    OkableIppPrintModal modal;
    modal.show_for_spool(test_spool(), [&](bool, const std::string&) { done = true; });
    modal.on_ok();

    REQUIRE(wait_until([&] { return done; }, 15000));
    REQUIRE(server.request_line() == "POST /printers/Canon_TS8350 HTTP/1.1");
}

namespace {

std::string upgrade_required_response() {
    return "HTTP/1.1 426 Upgrade Required\r\nUpgrade: TLS/1.2, HTTP/1.1\r\n"
           "Connection: Upgrade\r\nContent-Length: 0\r\n\r\n";
}

struct ServerCtx {
    helix::test::TestCert cert{"IP:127.0.0.1"};
    SSL_CTX* ctx = cert.make_server_ctx();
    ~ServerCtx() {
        SSL_CTX_free(ctx);
    }
};

/// Prints one label through IppPrinter to a loopback printer and waits for the verdict.
class IppJobFixture : public LVGLTestFixture {
  public:
    struct Result {
        bool done = false;
        bool ok = false;
        std::string message;
    };

    Result print_to(uint16_t port) {
        IppPrinter printer;
        printer.set_target("127.0.0.1", port, "ipp/print");
        printer.set_sheet_template(0);
        const auto& templates = label::get_sheet_templates();
        Result result;
        printer.print(LabelBitmap(64, 32), label::sheet_template_to_label_size(templates[0], 300),
                      [&](bool success, const std::string& message) {
                          result.ok = success;
                          result.message = message;
                          result.done = true;
                      });
        REQUIRE(wait_until([&] { return result.done; }, 30000));
        return result;
    }
};

std::string printer_uri(const char* scheme, uint16_t port) {
    return std::string(scheme) + "://127.0.0.1:" + std::to_string(port) + "/ipp/print";
}

} // namespace

TEST_CASE_METHOD(IppJobFixture, "An IPP job the printer answers with 426 is resent over TLS",
                 "[label][ipp_http][2034]") {
    ServerCtx tls;
    ScriptedServer server({{false, {upgrade_required_response()}}, {true, {final_ok_response()}}},
                          tls.ctx);

    const auto result = print_to(server.port());

    REQUIRE(server.request_count() == 2);
    CHECK(server.body(0).find(printer_uri("ipp", server.port())) != std::string::npos);
    CHECK(server.request_line(1) == "POST /ipp/print HTTP/1.1");
    CHECK(server.body(1).find(printer_uri("ipps", server.port())) != std::string::npos);
    CHECK(result.ok);
}

TEST_CASE_METHOD(IppJobFixture, "A printer still answering 426 over TLS fails the job",
                 "[label][ipp_http][2034]") {
    ServerCtx tls;
    ScriptedServer server(
        {{false, {upgrade_required_response()}}, {true, {upgrade_required_response()}}}, tls.ctx);

    const auto result = print_to(server.port());

    CHECK(server.request_count() == 2);
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("426") != std::string::npos);
}

TEST_CASE_METHOD(IppJobFixture, "An IPP job rejected with another HTTP error is not resent",
                 "[label][ipp_http][2034]") {
    ScriptedServer server(
        {"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});

    const auto result = print_to(server.port());

    CHECK_FALSE(result.ok);
    CHECK(result.message.find("400") != std::string::npos);
}
