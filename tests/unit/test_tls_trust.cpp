// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/scoped_env.h"
#include "../test_helpers/test_cert.h"
#include "system/tls_trust.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <netinet/in.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using helix::ScopedEnv;
using helix::test::TestCert;
using helix::tls::CaStore;
using helix::tls::find_ca_store;
using helix::tls::make_client_ctx;
using helix::tls::trusted_download;
namespace detail = helix::tls::detail;

namespace {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("helix-tls-test-" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string touch(const std::string& name, const std::string& body = "x") const {
        const std::string p = (path / name).string();
        std::ofstream(p) << body;
        return p;
    }
};

/// Runs a TLS handshake over loopback TCP between `client_ctx` and a server
/// presenting `server`. `sni` empty means an IP-literal connect (no SNI, as libhv
/// does for one). Returns whether the client accepted the server.
bool handshake(void* client_ctx, const TestCert& server, const std::string& sni) {
    SSL_CTX* sctx = server.make_server_ctx();

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(lfd, 1);
    getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len);
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(connect(cfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    int sfd = accept(lfd, nullptr, nullptr);
    fcntl(cfd, F_SETFL, O_NONBLOCK);
    fcntl(sfd, F_SETFL, O_NONBLOCK);

    SSL* c = SSL_new(static_cast<SSL_CTX*>(client_ctx));
    SSL* s = SSL_new(sctx);
    SSL_set_fd(c, cfd);
    SSL_set_fd(s, sfd);
    if (!sni.empty())
        SSL_set_tlsext_host_name(c, sni.c_str());

    int crc = -1, src = -1;
    for (int i = 0; i < 200 && crc != 1; ++i) {
        crc = SSL_connect(c);
        if (crc != 1 && SSL_get_error(c, crc) != SSL_ERROR_WANT_READ)
            break;
        if (src != 1)
            src = SSL_accept(s);
        usleep(1000);
    }

    SSL_free(c);
    SSL_free(s);
    SSL_CTX_free(sctx);
    close(cfd);
    close(sfd);
    close(lfd);
    return crc == 1;
}

/// A loopback HTTP server, TLS with `cert` unless `cert` is null. Answers each request
/// with `respond(path, raw_request)`, a complete HTTP response, and counts requests.
class TlsServer {
  public:
    using Responder = std::function<std::string(const std::string&, const std::string&)>;

    TlsServer(const TestCert* cert, Responder respond) : respond_(std::move(respond)) {
        if (cert) {
            ctx_ = cert->make_server_ctx();
        }
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(addr);
        bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        listen(fd_, 4);
        getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    TlsServer(const TestCert& cert, Responder respond) : TlsServer(&cert, std::move(respond)) {}
    ~TlsServer() {
        stop_ = true;
        thread_.join();
        close(fd_);
        SSL_CTX_free(ctx_);
    }
    std::string url(const std::string& path) const {
        return std::string(ctx_ ? "https" : "http") + "://127.0.0.1:" + std::to_string(port_) +
               path;
    }
    int port() const {
        return port_;
    }
    int hits() const {
        return hits_;
    }

  private:
    void serve() {
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (poll(&p, 1, 20) <= 0)
                continue;
            const int c = accept(fd_, nullptr, nullptr);
            SSL* ssl = ctx_ ? SSL_new(ctx_) : nullptr;
            if (ssl)
                SSL_set_fd(ssl, c);
            if (!ssl || SSL_accept(ssl) == 1) {
                std::string req;
                char buf[1024];
                long n;
                while (req.find("\r\n\r\n") == std::string::npos &&
                       (n = ssl ? SSL_read(ssl, buf, sizeof(buf)) : read(c, buf, sizeof(buf))) > 0)
                    req.append(buf, static_cast<size_t>(n));
                ++hits_;
                const size_t sp = req.find(' ');
                const std::string path = req.substr(sp + 1, req.find(' ', sp + 1) - sp - 1);
                const std::string out = respond_(path, req);
                if (ssl) {
                    SSL_write(ssl, out.data(), static_cast<int>(out.size()));
                    SSL_shutdown(ssl);
                } else {
                    (void)!write(c, out.data(), out.size());
                }
            }
            SSL_free(ssl);
            close(c);
        }
    }

    Responder respond_;
    SSL_CTX* ctx_ = nullptr;
    int fd_ = -1;
    int port_ = 0;
    std::atomic<int> hits_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

std::string ok_response(const std::string& body) {
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

std::string redirect_response(const std::string& location) {
    return "HTTP/1.1 302 Found\r\nLocation: " + location +
           "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
}

HttpRequestPtr get(const std::string& url) {
    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_GET;
    req->url = url;
    req->timeout = 5;
    return req;
}

} // namespace

TEST_CASE("find_ca_store: env beats bundled beats system", "[tls]") {
    TempDir dir;
    const std::string env_file = dir.touch("env.pem");
    const std::string sys_file = dir.touch("sys.pem");
    const std::string bundled = dir.touch("bundled.pem");
    const std::string missing = (dir.path / "missing.pem").string();

    SECTION("SSL_CERT_FILE and SSL_CERT_DIR win") {
        ScopedEnv f("SSL_CERT_FILE", env_file.c_str());
        ScopedEnv d("SSL_CERT_DIR", dir.path.c_str());
        const CaStore s = find_ca_store({sys_file}, bundled);
        CHECK(s.file == env_file);
        CHECK(s.dir == dir.path.string());
    }
    SECTION("an unreadable SSL_CERT_FILE falls through to the bundled file") {
        ScopedEnv f("SSL_CERT_FILE", missing.c_str());
        ScopedEnv d("SSL_CERT_DIR", nullptr);
        const CaStore s = find_ca_store({missing, sys_file}, bundled);
        CHECK(s.file == bundled);
        CHECK(s.dir.empty());
    }
    SECTION("an empty SSL_CERT_DIR falls through to the bundled file") {
        TempDir empty;
        ScopedEnv f("SSL_CERT_FILE", nullptr);
        ScopedEnv d("SSL_CERT_DIR", empty.path.c_str());
        const CaStore s = find_ca_store({sys_file}, bundled);
        CHECK(s.file == bundled);
        CHECK(s.dir.empty());
    }
    SECTION("the system bundle serves when there is no bundled file") {
        ScopedEnv f("SSL_CERT_FILE", nullptr);
        ScopedEnv d("SSL_CERT_DIR", nullptr);
        CHECK(find_ca_store({missing, sys_file}, missing).file == sys_file);
    }
    SECTION("an empty bundled file is skipped for the system bundle") {
        ScopedEnv f("SSL_CERT_FILE", nullptr);
        ScopedEnv d("SSL_CERT_DIR", nullptr);
        std::ofstream(dir.path / "empty.pem").close();
        CHECK(find_ca_store({sys_file}, (dir.path / "empty.pem").string()).file == sys_file);
    }
    SECTION("nothing readable reports no store") {
        ScopedEnv f("SSL_CERT_FILE", nullptr);
        ScopedEnv d("SSL_CERT_DIR", nullptr);
        CHECK(find_ca_store({missing}, missing).empty());
        CHECK(find_ca_store({}, "").empty());
    }
}

TEST_CASE("make_client_ctx verifies peers and ignores validity dates", "[tls]") {
    TempDir dir;
    auto* ctx = static_cast<SSL_CTX*>(make_client_ctx({dir.touch("ca.pem"), {}}));
    REQUIRE(ctx != nullptr);
    CHECK((SSL_CTX_get_verify_mode(ctx) & SSL_VERIFY_PEER) != 0);
    CHECK((X509_VERIFY_PARAM_get_flags(SSL_CTX_get0_param(ctx)) & X509_V_FLAG_NO_CHECK_TIME) != 0);
    SSL_CTX_free(ctx);
}

TEST_CASE("client ctx accepts only a trusted certificate for the host", "[tls]") {
    TempDir dir;
    TestCert server("DNS:helix.test,IP:127.0.0.1");

    SECTION("untrusted certificate is rejected") {
        TestCert other("DNS:other.test");
        auto* ctx = make_client_ctx({other.write_pem(dir.path), {}});
        CHECK_FALSE(handshake(ctx, server, "helix.test"));
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
    }
    SECTION("an unloadable CA file trusts nothing") {
        auto* ctx = make_client_ctx({dir.touch("empty.pem", ""), {}});
        REQUIRE(ctx != nullptr);
        CHECK_FALSE(handshake(ctx, server, "helix.test"));
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
    }
    SECTION("trusted certificate") {
        auto* ctx = make_client_ctx({server.write_pem(dir.path), {}});
        CHECK(handshake(ctx, server, "helix.test"));
        CHECK_FALSE(handshake(ctx, server, "evil.test"));
        CHECK(handshake(ctx, server, "")); // IP literal, matched against the peer address
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
    }
    SECTION("IP literal not in the certificate is rejected") {
        TestCert dns_only("DNS:helix.test");
        auto* ctx = make_client_ctx({dns_only.write_pem(dir.path), {}});
        CHECK_FALSE(handshake(ctx, dns_only, ""));
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
    }
    SECTION("a partial wildcard does not match") {
        TestCert partial("DNS:h*.example.test");
        auto* ctx = make_client_ctx({partial.write_pem(dir.path), {}});
        CHECK_FALSE(handshake(ctx, partial, "helix.example.test"));
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
    }
    SECTION("an expired certificate still verifies") {
        TestCert expired("DNS:helix.test", -60, -30);
        auto* ctx = make_client_ctx({expired.write_pem(dir.path), {}});
        CHECK(handshake(ctx, expired, "helix.test"));
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
    }
}

TEST_CASE("trusted_request verifies every hop of a redirect", "[tls]") {
    TempDir dir;
    TestCert trusted("IP:127.0.0.1");
    TestCert untrusted("IP:127.0.0.1");
    TlsServer evil(untrusted,
                   [](const std::string&, const std::string&) { return ok_response("evil"); });
    TlsServer mirror(trusted,
                     [](const std::string&, const std::string&) { return ok_response("mirror"); });
    TlsServer ours(trusted, [&](const std::string& path, const std::string&) {
        if (path == "/to-mirror")
            return redirect_response(mirror.url("/ok"));
        if (path == "/to-evil")
            return redirect_response(evil.url("/payload"));
        return ok_response("ok");
    });
    void* ctx = make_client_ctx({trusted.write_pem(dir.path), {}});

    auto direct = detail::trusted_request(get(ours.url("/ok")), ctx);
    REQUIRE(direct);
    CHECK(direct->body == "ok");

    auto hop = detail::trusted_request(get(ours.url("/to-mirror")), ctx);
    REQUIRE(hop);
    CHECK(hop->body == "mirror");

    CHECK_FALSE(detail::trusted_request(get(evil.url("/payload")), ctx));
    CHECK_FALSE(detail::trusted_request(get(ours.url("/to-evil")), ctx));

    SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
}

TEST_CASE("trusted_download refuses a server the CA store does not trust", "[tls]") {
    REQUIRE_FALSE(find_ca_store().empty());
    TempDir dir;
    TestCert self_signed("IP:127.0.0.1");
    TlsServer server(self_signed,
                     [](const std::string&, const std::string&) { return ok_response("payload"); });
    const std::string dest = (dir.path / "out.bin").string();

    CHECK(trusted_download(server.url("/f"), dest, nullptr) == 0);
    CHECK_FALSE(std::filesystem::exists(dest));
    CHECK_FALSE(std::filesystem::exists(dest + ".download"));
}

TEST_CASE("resolve_location handles absolute and relative redirects", "[tls]") {
    using detail::resolve_location;
    const std::string base = "https://h.test:8443/a/b/file?x=1";
    CHECK(resolve_location(base, "https://o.test/p") == "https://o.test/p");
    CHECK(resolve_location(base, "//o.test/p") == "https://o.test/p");
    CHECK(resolve_location(base, "/root?q=2") == "https://h.test:8443/root?q=2");
    CHECK(resolve_location(base, "sib") == "https://h.test:8443/a/b/sib");
    CHECK(resolve_location("https://h.test", "p") == "https://h.test/p");
}

TEST_CASE("trusted_request refuses to downgrade a redirect to plain http", "[tls]") {
    TempDir dir;
    TestCert trusted("IP:127.0.0.1");
    TlsServer plain(nullptr,
                    [](const std::string&, const std::string&) { return ok_response("plain"); });
    TlsServer ours(trusted, [&](const std::string&, const std::string&) {
        return redirect_response(plain.url("/x"));
    });
    void* ctx = make_client_ctx({trusted.write_pem(dir.path), {}});

    CHECK_FALSE(detail::trusted_request(get(ours.url("/down")), ctx));
    CHECK(ours.hits() == 1);
    CHECK(plain.hits() == 0);
    SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
}

TEST_CASE("trusted_request keeps credentials on its own origin only", "[tls]") {
    TempDir dir;
    TestCert trusted("IP:127.0.0.1");
    std::string mirror_request;
    std::mutex mu;
    TlsServer mirror(trusted, [&](const std::string&, const std::string& raw) {
        std::lock_guard<std::mutex> lock(mu);
        mirror_request = raw;
        return ok_response("mirror");
    });
    TlsServer ours(trusted, [&](const std::string& path, const std::string& raw) {
        if (path == "/elsewhere")
            return redirect_response(mirror.url("/m"));
        if (path == "/relative")
            return redirect_response("/echo");
        if (path == "/dir/sibling")
            return redirect_response("echo");
        if (path != "/echo" && path != "/dir/echo")
            return ok_response("wrong path " + path);
        return ok_response(raw.find("X-API-Key: k") != std::string::npos ? "key" : "nokey");
    });
    void* ctx = make_client_ctx({trusted.write_pem(dir.path), {}});
    auto with_key = [&](const std::string& url) {
        auto req = get(url);
        req->headers["X-API-Key"] = "k";
        req->headers["Authorization"] = "Bearer t";
        return req;
    };

    auto same = detail::trusted_request(with_key(ours.url("/relative")), ctx);
    REQUIRE(same);
    CHECK(same->body == "key");
    auto sibling = detail::trusted_request(with_key(ours.url("/dir/sibling")), ctx);
    REQUIRE(sibling);
    CHECK(sibling->body == "key");

    auto cross = detail::trusted_request(with_key(ours.url("/elsewhere")), ctx);
    REQUIRE(cross);
    CHECK(cross->body == "mirror");
    std::lock_guard<std::mutex> lock(mu);
    CHECK(mirror_request.find("X-API-Key") == std::string::npos);
    CHECK(mirror_request.find("Authorization") == std::string::npos);
    CHECK(mirror_request.find("Host: 127.0.0.1:" + std::to_string(mirror.port())) !=
          std::string::npos);
    SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
}

// ============================================================================
// TransferFailure: why a request came back empty (#1813)
// ============================================================================

namespace {

using helix::tls::FailureKind;
using helix::tls::TransferFailure;

/// A loopback TCP server that writes `reply` to each connection as soon as it is
/// accepted and closes it, without reading or speaking TLS.
class RawServer {
  public:
    explicit RawServer(std::string reply) : reply_(std::move(reply)) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(addr);
        bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        listen(fd_, 4);
        getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] {
            while (!stop_) {
                pollfd p{fd_, POLLIN, 0};
                if (poll(&p, 1, 20) <= 0)
                    continue;
                const int c = accept(fd_, nullptr, nullptr);
                (void)!write(c, reply_.data(), reply_.size());
                close(c);
            }
        });
    }
    ~RawServer() {
        stop_ = true;
        thread_.join();
        close(fd_);
    }
    std::string url(const std::string& scheme, const std::string& path) const {
        return scheme + "://127.0.0.1:" + std::to_string(port_) + path;
    }

  private:
    std::string reply_;
    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

/// A loopback port nothing listens on.
int closed_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    close(fd);
    return ntohs(addr.sin_port);
}

TransferFailure download_failure(const std::string& url, const std::string& dest, void* ctx) {
    TransferFailure failure;
    CHECK(detail::trusted_download(url, dest, nullptr, ctx, &failure) == 0);
    return failure;
}

} // namespace

TEST_CASE("trusted_download names why it failed", "[tls][transfer_failure]") {
    TempDir dir;
    TestCert trusted("IP:127.0.0.1");
    void* ctx = make_client_ctx({trusted.write_pem(dir.path), {}});
    const std::string dest = (dir.path / "out.bin").string();

    SECTION("an HTTP error status") {
        TlsServer server(trusted, [](const std::string&, const std::string&) {
            return std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: "
                               "close\r\n\r\n");
        });
        const auto f = download_failure(server.url("/missing"), dest, ctx);
        CHECK(f.kind == FailureKind::Http);
        CHECK(f.http_status == 404);
        CHECK(f.host == "127.0.0.1");
    }

    SECTION("a certificate the store does not trust") {
        TestCert stranger("IP:127.0.0.1");
        TlsServer server(stranger,
                         [](const std::string&, const std::string&) { return ok_response("x"); });
        const auto f = download_failure(server.url("/f"), dest, ctx);
        CHECK(f.kind == FailureKind::TlsCert);
        CHECK(f.code != 0);
        CHECK(f.http_status == -1);
    }

    SECTION("a trusted certificate for another host") {
        TestCert wrong_name("DNS:elsewhere.test");
        void* wrong_ctx = make_client_ctx({wrong_name.write_pem(dir.path), {}});
        TlsServer server(wrong_name,
                         [](const std::string&, const std::string&) { return ok_response("x"); });
        const auto f = download_failure(server.url("/f"), dest, wrong_ctx);
        CHECK(f.kind == FailureKind::TlsCert);
        CHECK(f.code == X509_V_ERR_HOSTNAME_MISMATCH);
        SSL_CTX_free(static_cast<SSL_CTX*>(wrong_ctx));
    }

    SECTION("a server that does not speak TLS") {
        RawServer server("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
        const auto f = download_failure(server.url("https", "/f"), dest, ctx);
        CHECK(f.kind == FailureKind::TlsHandshake);
    }

    SECTION("a refused connection") {
        const std::string url = "https://127.0.0.1:" + std::to_string(closed_port()) + "/f";
        const auto f = download_failure(url, dest, ctx);
        CHECK(f.kind == FailureKind::Connect);
        CHECK(f.code == ECONNREFUSED);
    }

    SECTION("a body shorter than its Content-Length") {
        TlsServer server(trusted, [](const std::string&, const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: "
                               "close\r\n\r\n0123456789");
        });
        const auto f = download_failure(server.url("/f"), dest, ctx);
        CHECK(f.kind == FailureKind::ShortBody);
        CHECK(f.http_status == 200);
    }

    SECTION("a 200 with an empty body") {
        TlsServer server(trusted,
                         [](const std::string&, const std::string&) { return ok_response(""); });
        const auto f = download_failure(server.url("/f"), dest, ctx);
        CHECK(f.kind == FailureKind::EmptyBody);
        CHECK(f.http_status == 200);
    }

    SECTION("a 200 with an empty close-delimited body") {
        TlsServer server(trusted, [](const std::string&, const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
        });
        const auto f = download_failure(server.url("/f"), dest, ctx);
        CHECK(f.kind == FailureKind::EmptyBody);
    }

    SECTION("a connection closed before any response") {
        RawServer server("");
        const auto f = download_failure(server.url("http", "/f"), dest, ctx);
        CHECK(f.kind == FailureKind::NoResponse);
    }

    SECTION("a redirect it refuses to follow") {
        TlsServer plain(nullptr,
                        [](const std::string&, const std::string&) { return ok_response("p"); });
        TlsServer server(trusted, [&](const std::string&, const std::string&) {
            return redirect_response(plain.url("/x"));
        });
        const auto f = download_failure(server.url("/f"), dest, ctx);
        CHECK(f.kind == FailureKind::Redirect);
    }

    SECTION("a destination it cannot create") {
        TlsServer server(trusted,
                         [](const std::string&, const std::string&) { return ok_response("x"); });
        const auto f =
            download_failure(server.url("/f"), (dir.path / "no-such-dir" / "out").string(), ctx);
        CHECK(f.kind == FailureKind::FileOpen);
        CHECK(f.code == ENOENT);
    }

    CHECK_FALSE(std::filesystem::exists(dest));
    CHECK_FALSE(std::filesystem::exists(dest + ".download"));
    SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
}

TEST_CASE("trusted_download restarts the file when libhv retries a dropped body",
          "[tls][transfer_failure]") {
    // libhv re-sends a request once when the connection drops mid-response, and the
    // retried response streams its body from the first byte again.
    TempDir dir;
    TestCert trusted("IP:127.0.0.1");
    void* ctx = make_client_ctx({trusted.write_pem(dir.path), {}});
    std::atomic<int> calls{0};
    TlsServer server(trusted, [&](const std::string&, const std::string&) {
        if (calls++ == 0)
            return std::string("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n01234");
        return std::string("HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: "
                           "close\r\n\r\n0123456789");
    });
    const std::string dest = (dir.path / "retried.bin").string();
    TransferFailure failure;
    CHECK(detail::trusted_download(server.url("/f"), dest, nullptr, ctx, &failure) == 10);
    CHECK(failure.kind == FailureKind::None);
    CHECK(calls == 2);
    std::ifstream in(dest);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(body == "0123456789");
    SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
}

TEST_CASE("trusted_download leaves the failure empty on success", "[tls][transfer_failure]") {
    TempDir dir;
    TestCert trusted("IP:127.0.0.1");
    void* ctx = make_client_ctx({trusted.write_pem(dir.path), {}});
    TlsServer server(trusted,
                     [](const std::string&, const std::string&) { return ok_response("payload"); });
    TransferFailure failure;
    CHECK(detail::trusted_download(server.url("/f"), (dir.path / "ok.bin").string(), nullptr, ctx,
                                   &failure) == 7);
    CHECK(failure.kind == FailureKind::None);
    SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
}

TEST_CASE("classify_send_failure separates the connection stages", "[tls][transfer_failure]") {
    using detail::classify_send_failure;
    using detail::SendTrace;
    const auto kind = [](int rc, SendTrace t, bool resolves) {
        return classify_send_failure(rc, t, resolves);
    };
    SendTrace none;
    SendTrace in_handshake;
    in_handshake.handshake_started = true;
    SendTrace after_handshake = in_handshake;
    after_handshake.handshake_done = true;
    SendTrace rejected = in_handshake;
    rejected.cert_error = X509_V_ERR_CERT_UNTRUSTED;

    CHECK(kind(-1, rejected, true).kind == FailureKind::TlsCert);
    CHECK(kind(-1, rejected, true).code == X509_V_ERR_CERT_UNTRUSTED);
    CHECK(kind(-1, in_handshake, true).kind == FailureKind::TlsHandshake);
    CHECK(kind(1100, after_handshake, true).kind == FailureKind::Timeout);
    CHECK(kind(1100, none, true).kind == FailureKind::Timeout);
    CHECK(kind(-1, none, false).kind == FailureKind::Dns);
    CHECK(kind(-ECONNREFUSED, none, true).kind == FailureKind::Connect);
    CHECK(kind(-ECONNREFUSED, none, true).code == ECONNREFUSED);
    CHECK(kind(-ETIMEDOUT, none, true).kind == FailureKind::Timeout);
    CHECK(kind(ECONNRESET, after_handshake, true).kind == FailureKind::Transfer);
    CHECK(kind(ECONNRESET, after_handshake, true).code == ECONNRESET);
    CHECK(kind(EAGAIN, after_handshake, true).kind == FailureKind::Timeout);
}

TEST_CASE("failure_kind_name gives every kind a fixed token", "[tls][transfer_failure]") {
    using helix::tls::failure_kind_name;
    CHECK(std::string(failure_kind_name(FailureKind::None)).empty());
    CHECK(std::string(failure_kind_name(FailureKind::Http)) == "http");
    CHECK(std::string(failure_kind_name(FailureKind::TlsCert)) == "tls_cert");
    CHECK(std::string(failure_kind_name(FailureKind::TlsHandshake)) == "tls_handshake");
    CHECK(std::string(failure_kind_name(FailureKind::Dns)) == "dns");
    CHECK(std::string(failure_kind_name(FailureKind::Connect)) == "connect");
    CHECK(std::string(failure_kind_name(FailureKind::Timeout)) == "timeout");
    CHECK(std::string(failure_kind_name(FailureKind::Transfer)) == "transfer");
    CHECK(std::string(failure_kind_name(FailureKind::NoResponse)) == "no_response");
    CHECK(std::string(failure_kind_name(FailureKind::ShortBody)) == "short_body");
    CHECK(std::string(failure_kind_name(FailureKind::EmptyBody)) == "empty_body");
    CHECK(std::string(failure_kind_name(FailureKind::Redirect)) == "redirect");
    CHECK(std::string(failure_kind_name(FailureKind::FileOpen)) == "file_open");
    CHECK(std::string(failure_kind_name(FailureKind::FileWrite)) == "file_write");
    CHECK(std::string(failure_kind_name(FailureKind::FileRename)) == "file_rename");
}
