// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/scoped_env.h"
#include "system/tls_trust.h"

#include <arpa/inet.h>
#include <atomic>
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

/// A self-signed P-256 certificate with the given subjectAltName, valid from
/// `not_before_days` to `not_after_days` relative to now.
struct TestCert {
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    TestCert(const char* san, long not_before_days = -1, long not_after_days = 30) {
        EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        EVP_PKEY_keygen_init(kctx);
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_X9_62_prime256v1);
        EVP_PKEY_keygen(kctx, &key);
        EVP_PKEY_CTX_free(kctx);

        cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), not_before_days * 86400);
        X509_gmtime_adj(X509_getm_notAfter(cert), not_after_days * 86400);
        X509_set_pubkey(cert, key);
        X509_NAME* subj = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(subj, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("helix-test"), -1, -1, 0);
        X509_set_issuer_name(cert, subj);
        X509V3_CTX v3;
        X509V3_set_ctx(&v3, cert, cert, nullptr, nullptr, 0);
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name, san);
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
        X509_sign(cert, key, EVP_sha256());
    }
    ~TestCert() {
        X509_free(cert);
        EVP_PKEY_free(key);
    }
    std::string write_pem(const TempDir& dir) const {
        const std::string p = (dir.path / "ca.pem").string();
        FILE* f = fopen(p.c_str(), "w");
        PEM_write_X509(f, cert);
        fclose(f);
        return p;
    }
};

/// Runs a TLS handshake over loopback TCP between `client_ctx` and a server
/// presenting `server`. `sni` empty means an IP-literal connect (no SNI, as libhv
/// does for one). Returns whether the client accepted the server.
bool handshake(void* client_ctx, const TestCert& server, const std::string& sni) {
    SSL_CTX* sctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_use_certificate(sctx, server.cert);
    SSL_CTX_use_PrivateKey(sctx, server.key);

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
            ctx_ = SSL_CTX_new(TLS_server_method());
            SSL_CTX_use_certificate(ctx_, cert->cert);
            SSL_CTX_use_PrivateKey(ctx_, cert->key);
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
        auto* ctx = make_client_ctx({other.write_pem(dir), {}});
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
        auto* ctx = make_client_ctx({server.write_pem(dir), {}});
        CHECK(handshake(ctx, server, "helix.test"));
        CHECK_FALSE(handshake(ctx, server, "evil.test"));
        CHECK(handshake(ctx, server, "")); // IP literal, matched against the peer address
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
    }
    SECTION("IP literal not in the certificate is rejected") {
        TestCert dns_only("DNS:helix.test");
        auto* ctx = make_client_ctx({dns_only.write_pem(dir), {}});
        CHECK_FALSE(handshake(ctx, dns_only, ""));
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
    }
    SECTION("a partial wildcard does not match") {
        TestCert partial("DNS:h*.example.test");
        auto* ctx = make_client_ctx({partial.write_pem(dir), {}});
        CHECK_FALSE(handshake(ctx, partial, "helix.example.test"));
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
    }
    SECTION("an expired certificate still verifies") {
        TestCert expired("DNS:helix.test", -60, -30);
        auto* ctx = make_client_ctx({expired.write_pem(dir), {}});
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
    void* ctx = make_client_ctx({trusted.write_pem(dir), {}});

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
    void* ctx = make_client_ctx({trusted.write_pem(dir), {}});

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
    void* ctx = make_client_ctx({trusted.write_pem(dir), {}});
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
